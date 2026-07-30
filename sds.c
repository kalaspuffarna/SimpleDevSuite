/* sds.c — SimpleDevSuite v2
 *
 * A small terminal dev environment inspired by VS Code:
 *
 *   - file tree (left, collapsible), tab bar (top), editor with line
 *     numbers, status bar
 *   - syntax highlighting: C, C++, Python, Bash, Rust, SQL, JS/TS, Go,
 *     Java, Lua, Ruby, PHP, JSON, TOML/YAML/INI, Makefile — optionally
 *     via tree-sitter when a grammar is installed (see below)
 *   - embedded pty terminal tabs (Alt+T) with scrollback and colors
 *   - git status markers in the tree and the branch in the status bar
 *   - config file and themes under ~/.config/sds/
 *   - undo/redo, selections, clipboard (with OSC 52 system-clipboard copy)
 *   - incremental find, replace, go-to-line
 *   - fuzzy quick-open (Ctrl+P), word-based autocomplete (Ctrl+Space)
 *   - auto-indent, bracket auto-close/skip/match-highlight
 *   - line ops: move, duplicate, delete, toggle comment, block (de)indent
 *   - bracketed paste, run-a-shell-command
 *
 * The mod key is Alt for app-level things; editing chords follow VS Code
 * where the terminal allows (see Alt+H in the app for the full list).
 * App-level keys are remappable in the config.
 *
 * Build:   cc -O2 -Wall -o sds sds.c -lncursesw -lutil
 *   with tree-sitter (optional, adds semantic highlighting):
 *          cc -O2 -Wall -DSDS_TREESITTER -o sds sds.c \
 *             $(pkg-config --cflags --libs tree-sitter) -lncursesw -lutil -ldl
 *   ./install.sh picks whichever of those applies automatically.
 *
 * Run:     ./sds [directory]
 *          ./sds --fetch-grammar cpp     install a tree-sitter grammar
 *
 * Tree-sitter is strictly optional and loaded at runtime: grammars live in
 * ~/.local/share/sds/grammars with their highlight queries beside them in
 * ~/.local/share/sds/queries. Any language without an installed grammar —
 * or the whole editor, when built without -DSDS_TREESITTER — falls back to
 * the built-in keyword lexer, which handles every language listed above.
 *
 * Known simplifications: editing is byte-based, so while the cursor will
 * not split a multi-byte character, wide (CJK) glyphs still count as one
 * column and can shift the rendering of a line; no multi-cursor; no LSP.
 * The terminal does not implement mouse reporting or sixel graphics.
 */

#define _XOPEN_SOURCE 700
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <locale.h>
#include <ncurses.h>
#include <fcntl.h>
#include <pty.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <zlib.h>
#ifdef SDS_TREESITTER
#include <dlfcn.h>
#include <tree_sitter/api.h>
#endif

#define MAX_TABS   32
#define UNDO_MAX   2000
#define QO_MAX     10000   /* quick-open file cap */
#define TABSTOP_MAX 16     /* upper bound on the configurable tab width */

/* Runtime-configurable (see ~/.config/sds/config). Both were compile-time
 * constants before the config system; they are still fixed after startup, so
 * buffers sized against them only need to allow for TABSTOP_MAX. */
static int tabstop = 4;            /* render width of a tab character */
static int tree_w  = 30;           /* file-tree sidebar width in columns */
static int tree_autohide = 80;     /* hide the sidebar below this COLS (0=never) */
static int tree_hidden = 0;        /* sidebar currently folded away */

/* ── key codes ────────────────────────────────────────────────────── */
/* Modified arrows/home/end arrive as CSI "1;<mod><dir>"; we register
 * every combination with define_key() so ncurses hands back one code.
 * mod: 2=Shift 3=Alt 4=Alt+Shift 5=Ctrl 6=Ctrl+Shift.
 * dir: 0=Up 1=Down 2=Left 3=Right 4=Home 5=End.                       */
#define MK(mod, dir) (2000 + (mod) * 10 + (dir))
enum { D_UP, D_DOWN, D_LEFT, D_RIGHT, D_HOME, D_END };
enum { K_PSTART = 2900, K_PEND, K_ADEL, K_AINS, K_NONE };
#define ALT(c)  (3000 + (c))
#undef  CTRL                      /* sys/ttydefaults.h (via pty.h) defines it */
#define CTRL(c) ((c) & 0x1f)

/* ── languages ────────────────────────────────────────────────────── */
enum { HA_DEF, HA_KW, HA_TYPE, HA_STR, HA_COM, HA_NUM, HA_PRE };
/* lexer states carried across lines */
enum { ST_NORM = 0, ST_BCOM, ST_TRI1, ST_TRI2 };

typedef struct {
    const char *name;
    const char *exts;   /* " c h " — space-delimited, spaces around each */
    const char *kw;     /* " if else " */
    const char *types;  /* second keyword class */
    const char *lc, *lc2;      /* line comments ("" = none)  */
    const char *bo, *bc;       /* block comment              */
    const char *t1, *t2;       /* multi-line string delims   */
    int soft_tabs;             /* Tab key inserts spaces     */
    int preproc;               /* '#'-lines are preprocessor */
    int nocase;                /* case-insensitive keywords  */
    int sq;                    /* single quote: 0 none, 1 char-literal, 2 string */
    int bq;                    /* backtick strings           */
} Lang;

static const Lang langs[] = {
  { "c", " c h ",
    " if else for while do switch case default return goto break continue"
    " sizeof typedef struct union enum const static extern inline volatile"
    " register restrict auto alignas alignof static_assert thread_local"
    " typeof typeof_unqual _Alignas _Alignof _Atomic _Generic _Noreturn"
    " _Static_assert _Thread_local defer ",
    " void char short int long float double signed unsigned bool _Bool"
    " _BitInt _Complex _Decimal32 _Decimal64 _Decimal128 _Imaginary"
    " size_t ssize_t ptrdiff_t intptr_t uintptr_t intmax_t uintmax_t"
    " wchar_t char8_t char16_t char32_t va_list"
    " int8_t int16_t int32_t int64_t uint8_t uint16_t uint32_t uint64_t"
    " int_least8_t int_least16_t int_least32_t int_least64_t"
    " int_fast8_t int_fast16_t int_fast32_t int_fast64_t"
    " FILE NULL true false nullptr errno ",
    "//", "", "/*", "*/", "", "", 0, 1, 0, 1, 0 },
  { "c++", " cpp cc cxx c++ hpp hh hxx h++ ipp tpp cu cuh ",
    " if else for while do switch case default return goto break continue"
    " sizeof typedef struct union enum const static extern inline volatile"
    " class namespace template typename public private protected virtual"
    " override final new delete this try catch throw using constexpr"
    " operator friend explicit mutable noexcept static_cast dynamic_cast"
    " reinterpret_cast const_cast decltype register restrict volatile"
    /* C++20/23: concepts, coroutines, modules — the gap that prompted this */
    " concept requires co_await co_yield co_return consteval constinit"
    " module import export "
    " alignas alignof static_assert thread_local typeid asm goto"
    " and and_eq bitand bitor compl not not_eq or or_eq xor xor_eq"
    " if_constexpr inline extern explicit ",
    " void char short int long float double signed unsigned bool auto"
    " char8_t char16_t char32_t wchar_t size_t ssize_t ptrdiff_t nullptr_t"
    " int8_t int16_t int32_t int64_t uint8_t uint16_t uint32_t uint64_t"
    " std string string_view vector map unordered_map set unordered_set"
    " pair tuple array deque list optional variant any span"
    " unique_ptr shared_ptr weak_ptr function initializer_list"
    " nullptr true false NULL ",
    "//", "", "/*", "*/", "", "", 0, 1, 0, 1, 0 },
  { "python", " py pyw ",
    " False None True and as assert async await break class continue def"
    " del elif else except finally for from global if import in is lambda"
    " nonlocal not or pass raise return try while with yield match case ",
    " print len range open str int float list dict set tuple bool bytes"
    " self super isinstance type Exception ValueError TypeError enumerate"
    " zip map filter sorted sum min max abs any all ",
    "#", "", "", "", "'''", "\"\"\"", 1, 0, 0, 2, 0 },
  { "bash", " sh bash zsh ",
    " if then else elif fi for while until do done case esac function in"
    " select time return exit break continue local export readonly"
    " declare set unset shift source alias trap ",
    " echo printf read cd pwd test true false eval exec kill wait sleep"
    " grep sed awk cat ls rm mv cp mkdir ",
    "#", "", "", "", "", "", 0, 0, 0, 2, 1 },
  { "rust", " rs ",
    " as break const continue crate dyn else enum extern fn for if impl in"
    " let loop match mod move mut pub ref return static struct super trait"
    " type unsafe use where while async await ",
    " i8 i16 i32 i64 i128 u8 u16 u32 u64 u128 f32 f64 usize isize bool"
    " char str String Vec Option Some None Result Ok Err Box self Self"
    " true false println print format vec ",
    "//", "", "/*", "*/", "", "", 1, 0, 0, 1, 0 },
  { "sql", " sql ",
    " select from where insert into values update set delete create table"
    " drop alter index view as join left right inner outer full cross on"
    " group by order having limit offset union all distinct and or not"
    " null is in exists between like case when then else end primary key"
    " foreign references default unique check constraint begin commit"
    " rollback transaction if replace with ",
    " int integer bigint smallint varchar char text date time timestamp"
    " datetime boolean decimal numeric float real double blob serial"
    " count sum avg min max coalesce ifnull now ",
    "--", "", "/*", "*/", "", "", 1, 0, 1, 2, 0 },
  { "javascript", " js jsx mjs cjs ",
    " break case catch class const continue debugger default delete do"
    " else export extends finally for function if import in instanceof"
    " let new of return static super switch this throw try typeof var"
    " void while with yield async await get set ",
    " true false null undefined console Number String Boolean Object"
    " Array Promise Map Set Symbol JSON Math document window require"
    " module NaN Infinity ",
    "//", "", "/*", "*/", "", "", 1, 0, 0, 2, 1 },
  { "typescript", " ts tsx ",
    " break case catch class const continue debugger default delete do"
    " else export extends finally for function if import in instanceof"
    " let new of return static super switch this throw try typeof var"
    " void while with yield async await get set interface type enum"
    " implements declare readonly namespace abstract public private"
    " protected keyof infer is asserts satisfies ",
    " true false null undefined any string number boolean object unknown"
    " never void console Promise Array Map Set Record Partial JSON Math ",
    "//", "", "/*", "*/", "", "", 1, 0, 0, 2, 1 },
  { "go", " go ",
    " break case chan const continue default defer else fallthrough for"
    " func go goto if import interface map package range return select"
    " struct switch type var ",
    " bool byte complex64 complex128 error float32 float64 int int8 int16"
    " int32 int64 rune string uint uint8 uint16 uint32 uint64 uintptr"
    " true false nil iota append cap close copy delete len make new panic"
    " print println recover any ",
    "//", "", "/*", "*/", "", "", 0, 0, 0, 1, 1 },
  { "java", " java ",
    " abstract assert break case catch class const continue default do"
    " else enum extends final finally for goto if implements import"
    " instanceof interface native new package private protected public"
    " return static strictfp super switch synchronized this throw throws"
    " transient try volatile while var record sealed permits yield ",
    " boolean byte char double float int long short void true false null"
    " String Object Integer Long Double Boolean List Map Set ArrayList"
    " HashMap System ",
    "//", "", "/*", "*/", "", "", 1, 0, 0, 1, 0 },
  { "lua", " lua ",
    " and break do else elseif end false for function goto if in local"
    " nil not or repeat return then true until while ",
    " print pairs ipairs table string math io os type tostring tonumber"
    " require self error pcall assert ",
    "--", "", "--[[", "]]", "", "", 1, 0, 0, 2, 0 },
  { "ruby", " rb ",
    " alias and begin break case class def do else elsif end ensure false"
    " for if in module next nil not or redo rescue retry return self"
    " super then true undef unless until when while yield ",
    " puts print require require_relative attr_accessor attr_reader"
    " attr_writer new raise lambda proc each map select inject ",
    "#", "", "", "", "", "", 1, 0, 0, 2, 0 },
  { "php", " php ",
    " echo print if else elseif while for foreach as function return"
    " class public private protected static new try catch finally throw"
    " namespace use require require_once include isset unset switch case"
    " default break continue do const abstract final interface implements"
    " extends instanceof match fn ",
    " true false null array string int float bool void this self parent ",
    "//", "#", "/*", "*/", "", "", 1, 0, 0, 2, 0 },
  { "json", " json ",
    " ", " true false null ",
    "", "", "", "", "", "", 1, 0, 0, 0, 0 },
  { "toml", " toml ini cfg conf ",
    " ", " true false ",
    "#", ";", "", "", "", "", 1, 0, 0, 2, 0 },
  { "yaml", " yml yaml ",
    " ", " true false null yes no ",
    "#", "", "", "", "", "", 1, 0, 0, 2, 0 },
  { "make", " mk makefile ",
    " ifeq ifneq ifdef ifndef else endif include define endef export ",
    " ", "#", "", "", "", "", "", 0, 0, 0, 0, 0 },
  { "text", "", " ", " ", "", "", "", "", "", "", 0, 0, 0, 0, 0 },
};
#define NLANGS ((int)(sizeof langs / sizeof *langs))
#define LANG_TEXT (&langs[NLANGS - 1])

static const Lang *lang_by_name(const char *name) {
    for (int i = 0; i < NLANGS; i++)
        if (!strcmp(langs[i].name, name)) return &langs[i];
    return LANG_TEXT;
}
static const Lang *lang_for(const char *path) {
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    if (!strcasecmp(base, "Makefile") || !strcasecmp(base, "GNUmakefile"))
        return lang_by_name("make");
    if (!strcasecmp(base, "CMakeLists.txt")) return lang_by_name("make");
    const char *dot = strrchr(base, '.');
    if (!dot || !dot[1]) return LANG_TEXT;
    char pat[32];
    snprintf(pat, sizeof pat, " %s ", dot + 1);
    for (char *p = pat; *p; p++) *p = (char)tolower((unsigned char)*p);
    for (int i = 0; i + 1 < NLANGS; i++)
        if (strstr(langs[i].exts, pat)) return &langs[i];
    return LANG_TEXT;
}

/* ── text buffer ──────────────────────────────────────────────────── */
typedef struct {
    char *s;
    int   len, cap;
    int   hst;                 /* lexer state at line start */
} Line;

typedef struct {
    int   type;                /* U_INS / U_DEL */
    int   y, x;
    char *t;
    int   tlen;
    int   group;
    int   cy, cx;              /* cursor before the action */
} URec;
enum { U_INS, U_DEL };

typedef struct Term Term;
typedef struct Pdf  Pdf;
enum { TAB_FILE, TAB_TERM, TAB_PDF };

typedef struct {
    int   kind;                /* TAB_FILE, TAB_TERM or TAB_PDF */
    Term *term;                /* set when kind == TAB_TERM */
    Pdf  *pdf;                 /* set when kind == TAB_PDF  */
    char  path[PATH_MAX];
    char  name[NAME_MAX + 1];
    const Lang *lang;
    Line *ln;
    int   n, cap;
    int   cy, cx;
    int   rowoff, coloff;
    int   subrow;              /* wrapped segments of ln[rowoff] scrolled past */
    int   dirty;
    int   ay, ax, sel;         /* selection anchor */
    int   hl_upto;             /* lines with valid hst: [0, hl_upto] */
    URec *undo; int nundo;
    URec *redo; int nredo;
    int   ver;                 /* bumped on every edit; drives reparse/caches */
#ifdef SDS_TREESITTER
    TSParser *ts_parser;
    TSTree   *ts_tree;
    uint32_t *ts_off;          /* byte offset of each line start */
    uint32_t  ts_bytes;
    int       ts_off_dirty;
    int       ts_ver;          /* `ver` the current tree was parsed from */
#endif
} Buf;

/* ── file tree ────────────────────────────────────────────────────── */
typedef struct Node {
    char *name, *path;
    int   is_dir, expanded, loaded;
    struct Node **kid;
    int   nkid;
    struct Node *parent;
    int   depth;
} Node;

/* ── globals ──────────────────────────────────────────────────────── */
static Buf  *tabs[MAX_TABS];
static int   ntabs = 0, cur = -1;

/* Split view. Each pane shows one tab; `cur` is always the focused pane's tab,
 * so every existing code path that reads `cur` keeps working unchanged and
 * only the few places that *set* it have to go through set_cur().           */
#define MAX_PANES 4
static int panes[MAX_PANES] = { -1, -1, -1, -1 };
static int npanes = 1, curpane = 0;

static Node *root = NULL;
static Node **vis = NULL;
static int   nvis = 0, viscap = 0, tsel = 0, toff = 0;

static char  msg[PATH_MAX + 64] = "";
static int   pending_close = 0, pending_quit = 0, show_help = 0;
static int   wrap = 0;                 /* soft-wrap long lines (Alt+Z) */
static int   g_wtw = 1;                /* text width in use, set by draw_editor */

static char *clip = NULL;      static int cliplen = 0;
static char  findq[256] = "";  static int find_show = 0;
static int   g_group = 0;      /* undo group counter */
static int   g_lastkind = 0;   /* for coalescing typed runs */
enum { AK_OTHER, AK_TYPE, AK_BS };

/* bracket-match highlight, recomputed each frame */
static int brk_y1 = -1, brk_x1, brk_y2, brk_x2;

/* ── utils ────────────────────────────────────────────────────────── */
static void tc_restore(void);   /* put back any palette slots we redefined */
static void die(const char *m) {
    tc_restore();
    endwin();
    fprintf(stderr, "%s\n", m);
    exit(1);
}
static void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n);
    if (!q) die("out of memory");
    return q;
}
static void *xmalloc(size_t n) { return xrealloc(NULL, n); }
static char *xstrdup(const char *s) {
    char *d = strdup(s);
    if (!d) die("out of memory");
    return d;
}
static void set_msg(const char *fmt, const char *a) {
    snprintf(msg, sizeof msg, fmt, a ? a : "");
}
static int word_ch(int c) { return isalnum((unsigned char)c) || c == '_' || (unsigned char)c >= 0x80; }
static int min2(int a, int b) { return a < b ? a : b; }
static int max2(int a, int b) { return a > b ? a : b; }

/* ── line ops ─────────────────────────────────────────────────────── */
static void line_grow(Line *l, int need) {
    if (l->cap >= need) return;
    l->cap = need < 32 ? 32 : need * 2;
    l->s = xrealloc(l->s, (size_t)l->cap);
}
/* ── buffer core ──────────────────────────────────────────────────── */
static void buf_insert_line(Buf *b, int at, const char *s, int len) {
    if (b->n == b->cap) {
        b->cap = b->cap ? b->cap * 2 : 64;
        b->ln = xrealloc(b->ln, (size_t)b->cap * sizeof(Line));
    }
    memmove(b->ln + at + 1, b->ln + at, (size_t)(b->n - at) * sizeof(Line));
    Line *l = &b->ln[at];
    l->len = len; l->cap = 0; l->s = NULL; l->hst = 0;
    line_grow(l, len ? len : 1);
    memcpy(l->s, s, (size_t)len);
    b->n++;
}
static void buf_del_line(Buf *b, int at) {
    free(b->ln[at].s);
    memmove(b->ln + at, b->ln + at + 1, (size_t)(b->n - at - 1) * sizeof(Line));
    b->n--;
}
static void term_free(Term *t);
static void pdf_free(Pdf *p);
static void urec_free(URec *r) { free(r->t); }
#ifdef SDS_TREESITTER
static void ts_forget(Buf *b);      /* drops any cached spans pointing at b */
#endif
static void buf_free(Buf *b) {
    for (int i = 0; i < b->n; i++) free(b->ln[i].s);
    for (int i = 0; i < b->nundo; i++) urec_free(&b->undo[i]);
    for (int i = 0; i < b->nredo; i++) urec_free(&b->redo[i]);
    free(b->undo); free(b->redo);
    free(b->ln);
    if (b->term) term_free(b->term);
    if (b->pdf)  pdf_free(b->pdf);
#ifdef SDS_TREESITTER
    ts_forget(b);
    if (b->ts_tree)   ts_tree_delete(b->ts_tree);
    if (b->ts_parser) ts_parser_delete(b->ts_parser);
    free(b->ts_off);
#endif
    free(b);
}
static Buf *buf_load(const char *path) {
    if (!path || !*path) return NULL;   /* also tells gcc -O1 it's non-null */
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    Buf *b = calloc(1, sizeof *b);
    if (!b) die("out of memory");
    snprintf(b->path, sizeof b->path, "%s", path);
    const char *slash = strrchr(path, '/');
    snprintf(b->name, sizeof b->name, "%s", slash ? slash + 1 : path);
    b->lang = lang_for(path);
    char *ln = NULL; size_t cap = 0; ssize_t r;
    while ((r = getline(&ln, &cap, f)) != -1) {
        while (r > 0 && (ln[r-1] == '\n' || ln[r-1] == '\r')) r--;
        buf_insert_line(b, b->n, ln, (int)r);
    }
    free(ln); fclose(f);
    if (b->n == 0) buf_insert_line(b, 0, "", 0);
    return b;
}
static int buf_save(Buf *b) {
    FILE *f = fopen(b->path, "w");
    if (!f) return -1;
    for (int i = 0; i < b->n; i++) {
        fwrite(b->ln[i].s, 1, (size_t)b->ln[i].len, f);
        fputc('\n', f);
    }
    fclose(f);
    b->dirty = 0;
    return 0;
}

/* ── raw edit primitives (no undo recording) ──────────────────────── */
/* Called from every edit path, so this is where the buffer version — and with
 * it the tree-sitter reparse and span cache — gets invalidated. */
static void hl_invalidate(Buf *b, int y) {
    if (y < b->hl_upto) b->hl_upto = y;
    b->ver++;
#ifdef SDS_TREESITTER
    b->ts_off_dirty = 1;
#endif
}

#ifdef SDS_TREESITTER
static void ts_offsets(Buf *b);
/* Byte offset of (y,x). Callers must use it before mutating the buffer —
 * the offset table describes the text as it currently stands. */
static uint32_t ts_byte_at(Buf *b, int y, int x) {
    ts_offsets(b);
    if (y < 0) return 0;
    if (y >= b->n) return b->ts_bytes;
    return b->ts_off[y] + (uint32_t)x;
}
/* Tell tree-sitter what changed so the next parse can reuse the old tree.
 * Without this a full reparse runs on every keystroke, which measured ~88ms
 * on a 3.7k-line file; with it, parsing is proportional to the edit. */
static void ts_note_edit(Buf *b, uint32_t sb, uint32_t ob, uint32_t nb,
                         int sy, int sx, int oy, int ox, int ny, int nx) {
    b->ts_off_dirty = 1;
    if (!b->ts_tree) return;
    TSInputEdit e;
    e.start_byte   = sb;
    e.old_end_byte = ob;
    e.new_end_byte = nb;
    e.start_point    = (TSPoint){ (uint32_t)sy, (uint32_t)sx };
    e.old_end_point  = (TSPoint){ (uint32_t)oy, (uint32_t)ox };
    e.new_end_point  = (TSPoint){ (uint32_t)ny, (uint32_t)nx };
    ts_tree_edit(b->ts_tree, &e);
}
#endif

static void ins_text(Buf *b, int y, int x, const char *t, int len,
                     int *ey, int *ex) {
#ifdef SDS_TREESITTER
    int sy = y, sx = x;
    uint32_t sb = ts_byte_at(b, y, x);
#endif
    int i = 0;
    while (i < len) {
        int j = i;
        while (j < len && t[j] != '\n') j++;
        int seg = j - i;
        Line *l = &b->ln[y];
        line_grow(l, l->len + seg);
        memmove(l->s + x + seg, l->s + x, (size_t)(l->len - x));
        memcpy(l->s + x, t + i, (size_t)seg);
        l->len += seg;
        x += seg;
        if (j < len) {                    /* newline: split */
            l = &b->ln[y];
            buf_insert_line(b, y + 1, l->s + x, l->len - x);
            b->ln[y].len = x;
            y++; x = 0;
        }
        i = j + 1;
    }
#ifdef SDS_TREESITTER
    ts_note_edit(b, sb, sb, sb + (uint32_t)len, sy, sx, sy, sx, y, x);
#endif
    if (ey) *ey = y;
    if (ex) *ex = x;
}
/* extract text of a (normalized) range into a malloc'd string */
static char *range_text(Buf *b, int y1, int x1, int y2, int x2, int *outlen) {
    size_t cap = 64, n = 0;
    char *t = xmalloc(cap);
    for (int y = y1; y <= y2; y++) {
        Line *l = &b->ln[y];
        int a = (y == y1) ? x1 : 0;
        int z = (y == y2) ? x2 : l->len;
        size_t need = n + (size_t)(z - a) + 2;
        if (need > cap) { cap = need * 2; t = xrealloc(t, cap); }
        memcpy(t + n, l->s + a, (size_t)(z - a));
        n += (size_t)(z - a);
        if (y < y2) t[n++] = '\n';
    }
    t[n] = 0;
    if (outlen) *outlen = (int)n;
    return t;
}
static void del_range_raw(Buf *b, int y1, int x1, int y2, int x2) {
#ifdef SDS_TREESITTER
    uint32_t sb = ts_byte_at(b, y1, x1), ob = ts_byte_at(b, y2, x2);
    ts_note_edit(b, sb, ob, sb, y1, x1, y2, x2, y1, x1);
#endif
    if (y1 == y2) {
        Line *l = &b->ln[y1];
        memmove(l->s + x1, l->s + x2, (size_t)(l->len - x2));
        l->len -= x2 - x1;
    } else {
        Line *a = &b->ln[y1], *z = &b->ln[y2];
        line_grow(a, x1 + (z->len - x2));
        memcpy(a->s + x1, z->s + x2, (size_t)(z->len - x2));
        a->len = x1 + (z->len - x2);
        for (int y = y2; y > y1; y--) buf_del_line(b, y);
    }
}
static void text_end(int y, int x, const char *t, int len, int *ey, int *ex) {
    for (int i = 0; i < len; i++) {
        if (t[i] == '\n') { y++; x = 0; }
        else x++;
    }
    *ey = y; *ex = x;
}

/* ── undo ─────────────────────────────────────────────────────────── */
static void redo_clear(Buf *b) {
    for (int i = 0; i < b->nredo; i++) urec_free(&b->redo[i]);
    b->nredo = 0;
}
static void push_undo(Buf *b, int type, int y, int x, char *t, int tlen) {
    if (b->nundo >= UNDO_MAX) {           /* drop the oldest group */
        int g = b->undo[0].group, k = 0;
        while (k < b->nundo && b->undo[k].group == g) urec_free(&b->undo[k++]);
        memmove(b->undo, b->undo + k, (size_t)(b->nundo - k) * sizeof(URec));
        b->nundo -= k;
    }
    b->undo = xrealloc(b->undo, (size_t)(b->nundo + 1) * sizeof(URec));
    URec *r = &b->undo[b->nundo++];
    r->type = type; r->y = y; r->x = x; r->t = t; r->tlen = tlen;
    r->group = g_group; r->cy = b->cy; r->cx = b->cx;
}
static void begin_action(int kind) {
    /* coalesce runs of plain typing / plain backspacing into one group */
    if (!(kind != AK_OTHER && kind == g_lastkind)) g_group++;
    g_lastkind = kind;
}
/* recorded edits — all user-visible modifications go through these */
static void edit_ins(Buf *b, int y, int x, const char *t, int len) {
    push_undo(b, U_INS, y, x, len ? memcpy(xmalloc((size_t)len + 1), t, (size_t)len) : xstrdup(""), len);
    if (len) b->undo[b->nundo - 1].t[len] = 0;
    int ey, ex;
    ins_text(b, y, x, t, len, &ey, &ex);
    b->cy = ey; b->cx = ex;
    b->dirty = 1;
    hl_invalidate(b, y);
    redo_clear(b);
}
static void edit_del(Buf *b, int y1, int x1, int y2, int x2) {
    if (y1 > y2 || (y1 == y2 && x1 > x2)) {
        int ty = y1, tx = x1; y1 = y2; x1 = x2; y2 = ty; x2 = tx;
    }
    int tlen;
    char *t = range_text(b, y1, x1, y2, x2, &tlen);
    push_undo(b, U_DEL, y1, x1, t, tlen);
    del_range_raw(b, y1, x1, y2, x2);
    b->cy = y1; b->cx = x1;
    b->dirty = 1;
    hl_invalidate(b, y1);
    redo_clear(b);
}
static void do_undo(Buf *b) {
    if (!b->nundo) { set_msg("nothing to undo", NULL); return; }
    int g = b->undo[b->nundo - 1].group;
    int rcy = 0, rcx = 0;
    while (b->nundo && b->undo[b->nundo - 1].group == g) {
        URec r = b->undo[--b->nundo];
        if (r.type == U_INS) {
            int ey, ex;
            text_end(r.y, r.x, r.t, r.tlen, &ey, &ex);
            del_range_raw(b, r.y, r.x, ey, ex);
        } else {
            ins_text(b, r.y, r.x, r.t, r.tlen, NULL, NULL);
        }
        hl_invalidate(b, r.y);
        rcy = r.cy; rcx = r.cx;
        b->redo = xrealloc(b->redo, (size_t)(b->nredo + 1) * sizeof(URec));
        b->redo[b->nredo++] = r;
    }
    b->cy = min2(rcy, b->n - 1);
    b->cx = min2(rcx, b->ln[b->cy].len);
    b->sel = 0; b->dirty = 1;
    g_lastkind = AK_OTHER;
}
static void do_redo(Buf *b) {
    if (!b->nredo) { set_msg("nothing to redo", NULL); return; }
    int g = b->redo[b->nredo - 1].group;
    while (b->nredo && b->redo[b->nredo - 1].group == g) {
        URec r = b->redo[--b->nredo];
        if (r.type == U_INS) {
            int ey, ex;
            ins_text(b, r.y, r.x, r.t, r.tlen, &ey, &ex);
            b->cy = ey; b->cx = ex;
        } else {
            int ey, ex;
            text_end(r.y, r.x, r.t, r.tlen, &ey, &ex);
            del_range_raw(b, r.y, r.x, ey, ex);
            b->cy = r.y; b->cx = r.x;
        }
        hl_invalidate(b, r.y);
        b->undo = xrealloc(b->undo, (size_t)(b->nundo + 1) * sizeof(URec));
        b->undo[b->nundo++] = r;
    }
    b->sel = 0; b->dirty = 1;
    g_lastkind = AK_OTHER;
}

/* ── selection ────────────────────────────────────────────────────── */
static int sel_norm(Buf *b, int *y1, int *x1, int *y2, int *x2) {
    if (!b->sel || (b->ay == b->cy && b->ax == b->cx)) return 0;
    if (b->ay < b->cy || (b->ay == b->cy && b->ax < b->cx)) {
        *y1 = b->ay; *x1 = b->ax; *y2 = b->cy; *x2 = b->cx;
    } else {
        *y1 = b->cy; *x1 = b->cx; *y2 = b->ay; *x2 = b->ax;
    }
    return 1;
}
static void sel_delete(Buf *b) {             /* assumes active selection */
    int y1, x1, y2, x2;
    if (sel_norm(b, &y1, &x1, &y2, &x2)) edit_del(b, y1, x1, y2, x2);
    b->sel = 0;
}

/* ── PDF ──────────────────────────────────────────────────────────────
 * A read-only PDF text extractor: enough to actually read a document in a
 * terminal, not a renderer. It indexes objects, decodes streams, walks the
 * page tree, and interprets the text-showing operators of a content stream.
 *
 * Two things make the output legible rather than a pile of letters. Glyph
 * codes go through the font's /ToUnicode map when it has one, which is what
 * turns a subset font back into words. And every run of text keeps the device
 * position it was drawn at, so the layout pass can rebuild rows and columns —
 * indented code and side-by-side columns survive the trip.
 *
 * The object index is built by scanning the whole file for "N G obj" rather
 * than by reading the xref. That costs one pass but sidesteps xref tables,
 * xref streams, incremental updates and damaged files all at once; objects
 * packed into /ObjStm are unpacked afterwards. Not handled: encryption, LZW,
 * images, and vertical writing. A page that yields no text says so.        */

#define PDF_MAXOBJ  400000     /* object-number ceiling, guards a bad parse */
#define PDF_MAXPAGE  20000
#define PDF_MAXFRAG 200000     /* text runs kept for one page */
#define PDF_MAXCOL    4000     /* widest line the layout will emit */

typedef struct { const char *p, *e; } PSpan;

typedef struct { uint32_t code; char u[10]; } PdfUni;   /* code → UTF-8 */

typedef struct {
    int      twobyte;          /* codes are 2 bytes (Identity-H and friends) */
    PdfUni  *uni; int nuni;    /* sorted /ToUnicode map */
    double  *w; int firstchar, nw;                      /* simple-font widths */
    double   dw;                                        /* CID default width  */
    struct { uint32_t lo, hi; double w; } *cw; int ncw; /* CID /W ranges      */
} PdfFont;

typedef struct { PSpan dict, res; double mb[4]; } PdfPage;

struct Pdf {
    char    *raw; size_t rawlen;
    PSpan   *obj; int nobj;
    char   **blob; int nblob;      /* decompressed /ObjStm bodies, kept alive */
    PdfPage *pg;  int npg;
    int      page;                 /* 0-based */
    int      encrypted;
};

/* ── lexing ───────────────────────────────────────────────────────── */
static int pdf_ws(int c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == 0;
}
static int pdf_delim(int c) {
    return c == '(' || c == ')' || c == '<' || c == '>' || c == '[' ||
           c == ']' || c == '{' || c == '}' || c == '/' || c == '%';
}
static const char *pdf_skip_ws(const char *p, const char *e) {
    for (;;) {
        while (p < e && pdf_ws((unsigned char)*p)) p++;
        if (p < e && *p == '%') {
            while (p < e && *p != '\n' && *p != '\r') p++;
            continue;
        }
        return p;
    }
}
/* Advance past exactly one object: dict, array, string, name, number or an
 * "N G R" reference (which has to be swallowed whole or the G would read as
 * the next value). */
static const char *pdf_skip_obj(const char *p, const char *e) {
    p = pdf_skip_ws(p, e);
    if (p >= e) return e;
    if (*p == '<' && p + 1 < e && p[1] == '<') {
        p += 2;
        for (;;) {
            p = pdf_skip_ws(p, e);
            if (p >= e) return e;
            if (*p == '>') return (p + 1 < e && p[1] == '>') ? p + 2 : p + 1;
            const char *q = pdf_skip_obj(p, e);
            if (q <= p) return e;
            p = q;
        }
    }
    if (*p == '[' || *p == '{') {
        char close = (*p == '[') ? ']' : '}';
        p++;
        for (;;) {
            p = pdf_skip_ws(p, e);
            if (p >= e) return e;
            if (*p == close) return p + 1;
            const char *q = pdf_skip_obj(p, e);
            if (q <= p) return e;
            p = q;
        }
    }
    if (*p == '(') {
        int d = 1;
        p++;
        while (p < e && d) {
            if (*p == '\\') { p += 2; continue; }
            if (*p == '(') d++;
            else if (*p == ')' && --d == 0) { p++; break; }
            p++;
        }
        return p;
    }
    if (*p == '<') { while (p < e && *p != '>') p++; return p < e ? p + 1 : e; }
    if (*p == '/') {
        p++;
        while (p < e && !pdf_ws((unsigned char)*p) && !pdf_delim((unsigned char)*p)) p++;
        return p;
    }
    if (*p == ']' || *p == '>' || *p == ')' || *p == '}') return p + 1;  /* stray */
    if (isdigit((unsigned char)*p) || *p == '+' || *p == '-' || *p == '.') {
        const char *q = p;
        while (q < e && (isdigit((unsigned char)*q) || *q == '+' || *q == '-' ||
                         *q == '.' || *q == 'e' || *q == 'E')) q++;
        const char *r = pdf_skip_ws(q, e);              /* "N G R"? */
        if (r < e && isdigit((unsigned char)*r)) {
            const char *s = r;
            while (s < e && isdigit((unsigned char)*s)) s++;
            const char *t = pdf_skip_ws(s, e);
            if (t < e && *t == 'R' &&
                (t + 1 >= e || pdf_ws((unsigned char)t[1]) || pdf_delim((unsigned char)t[1])))
                return t + 1;
        }
        return q;
    }
    {                                        /* bare keyword: true/false/null */
        const char *q = p;
        while (q < e && !pdf_ws((unsigned char)*q) && !pdf_delim((unsigned char)*q)) q++;
        return q > p ? q : p + 1;
    }
}
/* Read a /Name into `out`, decoding #xx escapes. NULL if there isn't one. */
static const char *pdf_name(const char *p, const char *e, char *out, size_t cap) {
    p = pdf_skip_ws(p, e);
    if (p >= e || *p != '/') return NULL;
    p++;
    size_t n = 0;
    while (p < e && !pdf_ws((unsigned char)*p) && !pdf_delim((unsigned char)*p)) {
        int c = (unsigned char)*p++;
        if (c == '#' && p + 1 < e && isxdigit((unsigned char)p[0]) &&
            isxdigit((unsigned char)p[1])) {
            char h[3] = { p[0], p[1], 0 };
            c = (int)strtol(h, NULL, 16);
            p += 2;
        }
        if (n + 1 < cap) out[n++] = (char)c;
    }
    out[n] = 0;
    return p;
}
static int pdf_isname(PSpan s, const char *want) {
    char nm[64];
    if (!s.p || !pdf_name(s.p, s.e, nm, sizeof nm)) return 0;
    return !strcmp(nm, want);
}
/* Value of /key in the dict starting at d.p, unresolved. */
static PSpan pdf_dget(PSpan d, const char *key) {
    PSpan none = { NULL, NULL };
    if (!d.p) return none;
    const char *p = pdf_skip_ws(d.p, d.e);
    if (p + 1 >= d.e || p[0] != '<' || p[1] != '<') return none;
    p += 2;
    for (;;) {
        p = pdf_skip_ws(p, d.e);
        if (p >= d.e || *p == '>') return none;
        char nm[64];
        const char *q = pdf_name(p, d.e, nm, sizeof nm);
        if (!q) return none;                              /* not a key: give up */
        const char *v = pdf_skip_ws(q, d.e);
        const char *ve = pdf_skip_obj(v, d.e);
        if (ve <= v) return none;
        if (!strcmp(nm, key)) { PSpan s = { v, ve }; return s; }
        p = ve;
    }
}
static int pdf_isref(PSpan s, int *num) {
    if (!s.p) return 0;
    const char *p = pdf_skip_ws(s.p, s.e);
    if (p >= s.e || !isdigit((unsigned char)*p)) return 0;
    char *q;
    long n = strtol(p, &q, 10);
    p = pdf_skip_ws(q, s.e);
    if (p >= s.e || !isdigit((unsigned char)*p)) return 0;
    strtol(p, &q, 10);
    p = pdf_skip_ws(q, s.e);
    if (p >= s.e || *p != 'R') return 0;
    *num = (int)n;
    return 1;
}
static PSpan pdf_get(Pdf *pdf, PSpan s) {          /* follow indirect refs */
    for (int i = 0; i < 32; i++) {
        int n;
        if (!pdf_isref(s, &n)) return s;
        if (n < 0 || n >= pdf->nobj || !pdf->obj[n].p) {
            PSpan none = { NULL, NULL };
            return none;
        }
        s = pdf->obj[n];
    }
    return s;
}
static PSpan pdf_dgetr(Pdf *pdf, PSpan d, const char *key) {
    return pdf_get(pdf, pdf_dget(d, key));
}
static double pdf_num(PSpan s, double dflt) {
    if (!s.p) return dflt;
    const char *p = pdf_skip_ws(s.p, s.e);
    if (p >= s.e) return dflt;
    if (!isdigit((unsigned char)*p) && *p != '-' && *p != '+' && *p != '.') return dflt;
    return atof(p);
}
static double pdf_dnum(Pdf *pdf, PSpan d, const char *key, double dflt) {
    return pdf_num(pdf_dgetr(pdf, d, key), dflt);
}
/* Iterate an array: on entry *p is inside the array, returns each element. */
static int pdf_arr_next(const char **p, const char *e, PSpan *out) {
    const char *s = pdf_skip_ws(*p, e);
    if (s >= e || *s == ']') return 0;
    const char *q = pdf_skip_obj(s, e);
    if (q <= s) return 0;
    out->p = s; out->e = q;
    *p = q;
    return 1;
}
static const char *pdf_arr_open(PSpan a) {   /* NULL when a isn't an array */
    if (!a.p) return NULL;
    const char *p = pdf_skip_ws(a.p, a.e);
    return (p < a.e && *p == '[') ? p + 1 : NULL;
}

/* ── stream filters ───────────────────────────────────────────────── */
static char *pdf_inflate(const char *in, size_t inlen, size_t *outlen) {
    z_stream z;
    memset(&z, 0, sizeof z);
    if (inflateInit(&z) != Z_OK) return NULL;
    size_t cap = inlen * 4 + 4096, n = 0;
    char *out = malloc(cap);
    if (!out) { inflateEnd(&z); return NULL; }
    z.next_in = (Bytef *)in;
    z.avail_in = (uInt)inlen;
    for (;;) {
        if (n == cap) {
            if (cap > ((size_t)256 << 20)) break;            /* runaway guard */
            char *t = realloc(out, cap * 2);
            if (!t) break;
            out = t; cap *= 2;
        }
        z.next_out = (Bytef *)out + n;
        z.avail_out = (uInt)(cap - n);
        int r = inflate(&z, Z_NO_FLUSH);
        n = cap - z.avail_out;
        if (r != Z_OK) break;                     /* end, or truncated: keep it */
        if (z.avail_in == 0 && z.avail_out != 0) break;
    }
    inflateEnd(&z);
    char *t = realloc(out, n + 1);
    if (!t) { free(out); return NULL; }
    t[n] = 0;
    *outlen = n;
    return t;
}
static char *pdf_a85(const char *in, size_t inlen, size_t *outlen) {
    char *out = malloc(inlen * 4 / 5 + 8);
    if (!out) return NULL;
    size_t n = 0;
    uint32_t acc = 0;
    int k = 0;
    for (size_t i = 0; i < inlen; i++) {
        int c = (unsigned char)in[i];
        if (pdf_ws(c)) continue;
        if (c == '~') break;
        if (c == 'z' && k == 0) { for (int j = 0; j < 4; j++) out[n++] = 0; continue; }
        if (c < '!' || c > 'u') continue;
        acc = acc * 85 + (uint32_t)(c - '!');
        if (++k == 5) {
            for (int j = 3; j >= 0; j--) out[n + j] = (char)(acc >> (8 * (3 - j)));
            n += 4; acc = 0; k = 0;
        }
    }
    if (k > 1) {                                   /* partial group, zero-padded */
        for (int j = k; j < 5; j++) acc = acc * 85 + 84;
        for (int j = 0; j < k - 1; j++) out[n++] = (char)(acc >> (8 * (3 - j)));
    }
    out[n] = 0;
    *outlen = n;
    return out;
}
static char *pdf_ahx(const char *in, size_t inlen, size_t *outlen) {
    char *out = malloc(inlen / 2 + 2);
    if (!out) return NULL;
    size_t n = 0;
    int hi = -1;
    for (size_t i = 0; i < inlen; i++) {
        int c = (unsigned char)in[i];
        if (c == '>') break;
        if (!isxdigit(c)) continue;
        int v = isdigit(c) ? c - '0' : (tolower(c) - 'a' + 10);
        if (hi < 0) hi = v;
        else { out[n++] = (char)((hi << 4) | v); hi = -1; }
    }
    if (hi >= 0) out[n++] = (char)(hi << 4);
    out[n] = 0;
    *outlen = n;
    return out;
}
/* PNG / TIFF predictors, as used by /ObjStm and xref streams. */
static void pdf_unpredict(char **buf, size_t *len, int pred, int colors,
                          int bpc, int columns) {
    if (pred < 2 || colors < 1 || bpc < 1 || columns < 1) return;
    int bpp = (colors * bpc + 7) / 8;
    size_t rowlen = ((size_t)columns * colors * bpc + 7) / 8;
    if (bpp < 1) bpp = 1;
    unsigned char *d = (unsigned char *)*buf;
    if (pred == 2) {                                       /* TIFF, 8bpc only */
        if (bpc != 8) return;
        for (size_t r = 0; r + rowlen <= *len; r += rowlen)
            for (size_t i = (size_t)bpp; i < rowlen; i++)
                d[r + i] = (unsigned char)(d[r + i] + d[r + i - bpp]);
        return;
    }
    size_t nrow = *len / (rowlen + 1), o = 0;
    unsigned char *out = malloc(nrow * rowlen + 1);
    if (!out) return;
    for (size_t r = 0; r < nrow; r++) {
        unsigned char ft = d[r * (rowlen + 1)];
        const unsigned char *src = d + r * (rowlen + 1) + 1;
        unsigned char *cur = out + o, *up = (r ? cur - rowlen : NULL);
        for (size_t i = 0; i < rowlen; i++) {
            int a = (i >= (size_t)bpp) ? cur[i - bpp] : 0;
            int b = up ? up[i] : 0;
            int c = (up && i >= (size_t)bpp) ? up[i - bpp] : 0;
            int x = src[i];
            switch (ft) {
                case 1: x += a; break;
                case 2: x += b; break;
                case 3: x += (a + b) / 2; break;
                case 4: {
                    int p = a + b - c;
                    int pa = p > a ? p - a : a - p;
                    int pb = p > b ? p - b : b - p;
                    int pc = p > c ? p - c : c - p;
                    x += (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
                    break;
                }
                default: break;
            }
            cur[i] = (unsigned char)x;
        }
        o += rowlen;
    }
    out[o] = 0;
    free(*buf);
    *buf = (char *)out;
    *len = o;
}
/* Decode the stream attached to the object whose body is `d`. */
static char *pdf_stream(Pdf *pdf, PSpan d, size_t *outlen) {
    *outlen = 0;
    const char *p = pdf_skip_ws(d.p, d.e);
    if (p + 1 >= d.e || p[0] != '<' || p[1] != '<') return NULL;
    const char *dend = pdf_skip_obj(p, d.e);
    PSpan dict = { p, dend };
    p = pdf_skip_ws(dend, d.e);
    if (d.e - p < 6 || memcmp(p, "stream", 6) != 0) return NULL;
    p += 6;
    if (p < d.e && *p == '\r') p++;
    if (p < d.e && *p == '\n') p++;

    long n = (long)pdf_dnum(pdf, dict, "Length", -1);
    if (n < 0 || p + n > d.e) {                  /* bad or missing /Length */
        const char *stop = NULL;
        for (const char *q = p; q + 9 <= d.e; q++)
            if (*q == 'e' && memcmp(q, "endstream", 9) == 0) { stop = q; break; }
        n = stop ? (long)(stop - p) : (long)(d.e - p);
        while (n > 0 && (p[n - 1] == '\n' || p[n - 1] == '\r')) n--;
    }
    if (n < 0) return NULL;
    char *data = malloc((size_t)n + 1);
    if (!data) return NULL;
    memcpy(data, p, (size_t)n);
    data[n] = 0;
    size_t dl = (size_t)n;

    PSpan f = pdf_dgetr(pdf, dict, "Filter");
    const char *fp = f.p, *fe = f.e;
    int arr = 0;
    if (fp) {
        const char *s = pdf_skip_ws(fp, fe);
        if (s < fe && *s == '[') { arr = 1; fp = s + 1; }
    }
    for (int k = 0; k < 8 && fp && fp < fe; k++) {
        char nm[64];
        const char *q = pdf_name(fp, fe, nm, sizeof nm);
        if (!q) break;
        fp = q;
        char *nd = NULL;
        size_t nl = 0;
        if (!strcmp(nm, "FlateDecode") || !strcmp(nm, "Fl"))
            nd = pdf_inflate(data, dl, &nl);
        else if (!strcmp(nm, "ASCII85Decode") || !strcmp(nm, "A85"))
            nd = pdf_a85(data, dl, &nl);
        else if (!strcmp(nm, "ASCIIHexDecode") || !strcmp(nm, "AHx"))
            nd = pdf_ahx(data, dl, &nl);
        else break;                       /* LZW, DCT, … : hand back what we have */
        free(data);
        if (!nd) return NULL;
        data = nd; dl = nl;
        if (!arr) break;
    }
    PSpan parms = pdf_dgetr(pdf, dict, "DecodeParms");
    int pred = (int)pdf_dnum(pdf, parms, "Predictor", 1);
    if (pred > 1)
        pdf_unpredict(&data, &dl, pred,
                      (int)pdf_dnum(pdf, parms, "Colors", 1),
                      (int)pdf_dnum(pdf, parms, "BitsPerComponent", 8),
                      (int)pdf_dnum(pdf, parms, "Columns", 1));
    *outlen = dl;
    return data;
}

/* ── object index ─────────────────────────────────────────────────── */
static void pdf_obj_room(Pdf *pdf, int num) {
    if (num < pdf->nobj) return;
    int want = num + 1;
    pdf->obj = xrealloc(pdf->obj, (size_t)want * sizeof *pdf->obj);
    memset(pdf->obj + pdf->nobj, 0, (size_t)(want - pdf->nobj) * sizeof *pdf->obj);
    pdf->nobj = want;
}
static void pdf_index(Pdf *pdf) {
    const char *b = pdf->raw, *e = b + pdf->rawlen;
    int *num = NULL;
    const char **hdr = NULL, **body = NULL;
    int n = 0, cap = 0;

    /* Every "<num> <gen> obj" in the file, in file order. */
    for (const char *p = b; p + 3 <= e; p++) {
        if (p[0] != 'o' || p[1] != 'b' || p[2] != 'j') continue;
        if (p + 3 < e && !pdf_ws((unsigned char)p[3]) && !pdf_delim((unsigned char)p[3]))
            continue;
        const char *q = p;
        while (q > b && pdf_ws((unsigned char)q[-1])) q--;
        if (q == p) continue;                        /* "obj" needs space before */
        const char *ge = q;
        while (q > b && isdigit((unsigned char)q[-1])) q--;
        if (q == ge) continue;
        const char *gs = q;
        while (q > b && pdf_ws((unsigned char)q[-1])) q--;
        if (q == gs) continue;
        const char *ne = q;
        while (q > b && isdigit((unsigned char)q[-1])) q--;
        if (q == ne) continue;
        if (q > b && !pdf_ws((unsigned char)q[-1]) && !pdf_delim((unsigned char)q[-1]))
            continue;
        long id = strtol(q, NULL, 10);
        if (id < 0 || id > PDF_MAXOBJ) continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 256;
            num  = xrealloc(num,  (size_t)cap * sizeof *num);
            hdr  = xrealloc(hdr,  (size_t)cap * sizeof *hdr);
            body = xrealloc(body, (size_t)cap * sizeof *body);
        }
        num[n] = (int)id;
        hdr[n] = q;
        body[n] = p + 3;
        n++;
        p += 2;
    }
    /* Each object runs to its "endobj", or to the next header if it has none.
     * Later definitions win, which is what an incremental update wants. */
    for (int i = 0; i < n; i++) {
        const char *limit = (i + 1 < n) ? hdr[i + 1] : e;
        const char *end = limit;
        for (const char *q = body[i]; q + 6 <= limit; q++)
            if (*q == 'e' && memcmp(q, "endobj", 6) == 0) { end = q; break; }
        pdf_obj_room(pdf, num[i]);
        pdf->obj[num[i]].p = body[i];
        pdf->obj[num[i]].e = end;
    }
    free(num); free(hdr); free(body);

    /* Unpack /ObjStm containers. Objects already defined at the top level win,
     * so a hybrid file's newer definitions are not clobbered. */
    for (int i = 0; i < pdf->nobj; i++) {
        PSpan d = pdf->obj[i];
        if (!d.p || !pdf_isname(pdf_dget(d, "Type"), "ObjStm")) continue;
        size_t dl;
        char *data = pdf_stream(pdf, d, &dl);
        if (!data) continue;
        int cnt = (int)pdf_dnum(pdf, d, "N", 0);
        long first = (long)pdf_dnum(pdf, d, "First", 0);
        if (cnt <= 0 || cnt > PDF_MAXOBJ || first <= 0 || (size_t)first > dl) {
            free(data);
            continue;
        }
        pdf->blob = xrealloc(pdf->blob, (size_t)(pdf->nblob + 1) * sizeof *pdf->blob);
        pdf->blob[pdf->nblob++] = data;

        int *on = xmalloc((size_t)cnt * sizeof *on);
        long *oo = xmalloc((size_t)cnt * sizeof *oo);
        const char *hp = data, *he = data + first;
        int got = 0;
        for (; got < cnt; got++) {
            char *q;
            hp = pdf_skip_ws(hp, he);
            if (hp >= he || !isdigit((unsigned char)*hp)) break;
            on[got] = (int)strtol(hp, &q, 10);
            hp = pdf_skip_ws(q, he);
            if (hp >= he || !isdigit((unsigned char)*hp)) break;
            oo[got] = strtol(hp, &q, 10);
            hp = q;
        }
        for (int k = 0; k < got; k++) {
            long s = first + oo[k];
            long en = (k + 1 < got) ? first + oo[k + 1] : (long)dl;
            if (on[k] < 0 || on[k] > PDF_MAXOBJ || s < 0 || en > (long)dl || en < s)
                continue;
            pdf_obj_room(pdf, on[k]);
            if (pdf->obj[on[k]].p) continue;
            pdf->obj[on[k]].p = data + s;
            pdf->obj[on[k]].e = data + en;
        }
        free(on); free(oo);
    }
}

/* ── page tree ────────────────────────────────────────────────────── */
static void pdf_rect(PSpan a, double *out) {
    const char *p = pdf_arr_open(a);
    if (!p) return;
    PSpan v;
    for (int i = 0; i < 4 && pdf_arr_next(&p, a.e, &v); i++) out[i] = pdf_num(v, out[i]);
    if (out[2] < out[0]) { double t = out[0]; out[0] = out[2]; out[2] = t; }
    if (out[3] < out[1]) { double t = out[1]; out[1] = out[3]; out[3] = t; }
}
static void pdf_addpage(Pdf *pdf, PSpan dict, PSpan res, const double *mb) {
    if (pdf->npg >= PDF_MAXPAGE) return;
    if ((pdf->npg & 63) == 0)
        pdf->pg = xrealloc(pdf->pg, (size_t)(pdf->npg + 64) * sizeof *pdf->pg);
    PdfPage *g = &pdf->pg[pdf->npg++];
    g->dict = dict;
    g->res = res;
    memcpy(g->mb, mb, 4 * sizeof(double));
}
static void pdf_walk(Pdf *pdf, PSpan node, PSpan res, const double *mb,
                     int depth) {
    if (depth > 48 || pdf->npg >= PDF_MAXPAGE || !node.p) return;
    PSpan r = pdf_dgetr(pdf, node, "Resources");
    if (r.p) res = r;
    double box[4] = { mb[0], mb[1], mb[2], mb[3] };
    PSpan m = pdf_dgetr(pdf, node, "MediaBox");
    if (m.p) pdf_rect(m, box);
    PSpan kids = pdf_dgetr(pdf, node, "Kids");
    const char *p = pdf_arr_open(kids);
    if (p) {
        PSpan k;
        while (pdf_arr_next(&p, kids.e, &k))
            pdf_walk(pdf, pdf_get(pdf, k), res, box, depth + 1);
        return;
    }
    pdf_addpage(pdf, node, res, box);
}
static void pdf_pages(Pdf *pdf) {
    const char *b = pdf->raw, *e = b + pdf->rawlen;
    PSpan root = { NULL, NULL };
    for (const char *p = e - 7; p >= b && !root.p; p--) {     /* newest trailer */
        if (*p != 't' || memcmp(p, "trailer", 7) != 0) continue;
        PSpan t = { p + 7, e };
        PSpan r = pdf_dget(t, "Root");
        if (r.p) root = pdf_get(pdf, r);
    }
    if (!root.p)                              /* xref-stream file, or damaged */
        for (int i = 0; i < pdf->nobj; i++) {
            if (!pdf->obj[i].p) continue;
            if (pdf_isname(pdf_dget(pdf->obj[i], "Type"), "Catalog")) {
                root = pdf->obj[i];
                break;
            }
            PSpan r = pdf_dget(pdf->obj[i], "Root");
            if (r.p && pdf_isname(pdf_dget(pdf->obj[i], "Type"), "XRef")) {
                root = pdf_get(pdf, r);
                if (root.p) break;
            }
        }
    double mb[4] = { 0, 0, 612, 792 };
    PSpan none = { NULL, NULL };
    PSpan pages = pdf_dgetr(pdf, root, "Pages");
    if (pages.p) pdf_walk(pdf, pages, none, mb, 0);
    if (pdf->npg == 0)                        /* no usable tree: sweep for leaves */
        for (int i = 0; i < pdf->nobj; i++)
            if (pdf->obj[i].p && pdf_isname(pdf_dget(pdf->obj[i], "Type"), "Page"))
                pdf_addpage(pdf, pdf->obj[i],
                            pdf_dgetr(pdf, pdf->obj[i], "Resources"), mb);
}

/* ── fonts ────────────────────────────────────────────────────────── */
static int pdf_utf8(uint32_t c, char *out) {
    if (c < 0x80)     { out[0] = (char)c; return 1; }
    if (c < 0x800)    { out[0] = (char)(0xc0 | (c >> 6));
                        out[1] = (char)(0x80 | (c & 0x3f)); return 2; }
    if (c < 0x10000)  { out[0] = (char)(0xe0 | (c >> 12));
                        out[1] = (char)(0x80 | ((c >> 6) & 0x3f));
                        out[2] = (char)(0x80 | (c & 0x3f)); return 3; }
    out[0] = (char)(0xf0 | (c >> 18));
    out[1] = (char)(0x80 | ((c >> 12) & 0x3f));
    out[2] = (char)(0x80 | ((c >> 6) & 0x3f));
    out[3] = (char)(0x80 | (c & 0x3f));
    return 4;
}
/* A <hex string> into bytes. Returns the byte count, -1 if it isn't one. */
static int pdf_hexstr(const char **pp, const char *e, unsigned char *out, int cap) {
    const char *p = pdf_skip_ws(*pp, e);
    if (p >= e || *p != '<') return -1;
    p++;
    int n = 0, hi = -1;
    for (; p < e && *p != '>'; p++) {
        int c = (unsigned char)*p;
        if (!isxdigit(c)) continue;
        int v = isdigit(c) ? c - '0' : (tolower(c) - 'a' + 10);
        if (hi < 0) hi = v;
        else { if (n < cap) out[n++] = (unsigned char)((hi << 4) | v); hi = -1; }
    }
    if (hi >= 0 && n < cap) out[n++] = (unsigned char)(hi << 4);
    *pp = (p < e) ? p + 1 : e;
    return n;
}
static uint32_t pdf_becode(const unsigned char *b, int n) {
    uint32_t v = 0;
    for (int i = 0; i < n && i < 4; i++) v = (v << 8) | b[i];
    return v;
}
/* UTF-16BE destination text (what /ToUnicode maps to) into UTF-8. */
static void pdf_u16(const unsigned char *s, int n, char *out, size_t cap) {
    size_t o = 0;
    for (int i = 0; i + 1 < n; i += 2) {
        uint32_t c = (uint32_t)(s[i] << 8 | s[i + 1]);
        if (c >= 0xd800 && c < 0xdc00 && i + 3 < n) {
            uint32_t lo = (uint32_t)(s[i + 2] << 8 | s[i + 3]);
            if (lo >= 0xdc00 && lo < 0xe000) {
                c = 0x10000 + ((c - 0xd800) << 10) + (lo - 0xdc00);
                i += 2;
            }
        }
        if (c == 0) continue;
        char tmp[4];
        int k = pdf_utf8(c, tmp);
        if (o + (size_t)k + 1 > cap) break;
        memcpy(out + o, tmp, (size_t)k);
        o += (size_t)k;
    }
    out[o] = 0;
}
static int pdf_uni_cmp(const void *a, const void *b) {
    uint32_t x = ((const PdfUni *)a)->code, y = ((const PdfUni *)b)->code;
    return x < y ? -1 : x > y;
}
static void pdf_uni_add(PdfFont *f, int *cap, uint32_t code, const char *u) {
    if (!*u) return;
    if (f->nuni == *cap) {
        *cap = *cap ? *cap * 2 : 128;
        f->uni = xrealloc(f->uni, (size_t)*cap * sizeof *f->uni);
    }
    f->uni[f->nuni].code = code;
    snprintf(f->uni[f->nuni].u, sizeof f->uni[f->nuni].u, "%s", u);
    f->nuni++;
}
/* Parse a /ToUnicode CMap: codespace width, bfchar and bfrange sections. */
static void pdf_cmap(PdfFont *f, const char *p, const char *e) {
    int cap = 0;
    while (p < e) {
        const char *q = p;
        while (q < e && !isalpha((unsigned char)*q) && *q != '_') q++;
        if (q >= e) break;
        const char *w = q;
        while (q < e && (isalnum((unsigned char)*q) || *q == '_')) q++;
        size_t wl = (size_t)(q - w);

        if (wl == 19 && !memcmp(w, "begincodespacerange", 19)) {
            unsigned char lo[16];
            const char *s = q;
            int n = pdf_hexstr(&s, e, lo, sizeof lo);
            if (n >= 2) f->twobyte = 1;
            p = q;
        } else if (wl == 11 && !memcmp(w, "beginbfchar", 11)) {
            const char *s = q;
            for (int i = 0; i < 65536; i++) {
                unsigned char src[8], dst[64];
                const char *t = pdf_skip_ws(s, e);
                if (t >= e || *t != '<') break;
                int sn = pdf_hexstr(&s, e, src, sizeof src);
                if (sn < 1) break;
                if (sn >= 2) f->twobyte = 1;
                t = pdf_skip_ws(s, e);
                char u[24] = "";
                if (t < e && *t == '<') {
                    int dn = pdf_hexstr(&s, e, dst, sizeof dst);
                    if (dn > 0) pdf_u16(dst, dn, u, sizeof u);
                } else {                        /* a glyph /name destination */
                    char nm[64];
                    const char *r = pdf_name(t, e, nm, sizeof nm);
                    if (!r) break;
                    s = r;
                    if (!strncmp(nm, "uni", 3) && strlen(nm) >= 7) {
                        unsigned c = (unsigned)strtol(nm + 3, NULL, 16);
                        u[pdf_utf8(c, u)] = 0;
                    } else if (!nm[1]) { u[0] = nm[0]; u[1] = 0; }
                }
                pdf_uni_add(f, &cap, pdf_becode(src, sn), u);
            }
            p = s;
        } else if (wl == 12 && !memcmp(w, "beginbfrange", 12)) {
            const char *s = q;
            for (int i = 0; i < 65536; i++) {
                unsigned char a[8], b[8], dst[64];
                const char *t = pdf_skip_ws(s, e);
                if (t >= e || *t != '<') break;
                int an = pdf_hexstr(&s, e, a, sizeof a);
                int bn = pdf_hexstr(&s, e, b, sizeof b);
                if (an < 1 || bn < 1) break;
                if (an >= 2) f->twobyte = 1;
                uint32_t lo = pdf_becode(a, an), hi = pdf_becode(b, bn);
                if (hi < lo || hi - lo > 65535) hi = lo;
                t = pdf_skip_ws(s, e);
                if (t < e && *t == '[') {              /* one destination each */
                    s = t + 1;
                    for (uint32_t c = lo; c <= hi; c++) {
                        const char *u2 = pdf_skip_ws(s, e);
                        if (u2 >= e || *u2 != '<') break;
                        int dn = pdf_hexstr(&s, e, dst, sizeof dst);
                        char u[24] = "";
                        if (dn > 0) pdf_u16(dst, dn, u, sizeof u);
                        pdf_uni_add(f, &cap, c, u);
                    }
                    const char *u2 = pdf_skip_ws(s, e);
                    if (u2 < e && *u2 == ']') s = u2 + 1;
                } else if (t < e && *t == '<') {       /* consecutive from base */
                    int dn = pdf_hexstr(&s, e, dst, sizeof dst);
                    if (dn < 2) continue;
                    for (uint32_t c = lo; c <= hi; c++) {
                        char u[24] = "";
                        pdf_u16(dst, dn, u, sizeof u);
                        pdf_uni_add(f, &cap, c, u);
                        for (int k = dn - 1; k >= 0; k--)   /* ++ the last unit */
                            if (++dst[k]) break;
                    }
                } else break;
            }
            p = s;
        } else p = q;
    }
    if (f->nuni > 1) qsort(f->uni, (size_t)f->nuni, sizeof *f->uni, pdf_uni_cmp);
}
/* CP1252's 0x80-0x9f block, the one range where it differs from Latin-1. */
static const uint16_t pdf_win1252[32] = {
    0x20ac, 0x0081, 0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021,
    0x02c6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008d, 0x017d, 0x008f,
    0x0090, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014,
    0x02dc, 0x2122, 0x0161, 0x203a, 0x0153, 0x009d, 0x017e, 0x0178,
};
static const char *pdf_glyph(PdfFont *f, uint32_t code, char *tmp) {
    if (f->nuni) {
        int lo = 0, hi = f->nuni - 1;
        while (lo <= hi) {
            int m = (lo + hi) / 2;
            if (f->uni[m].code == code) return f->uni[m].u;
            if (f->uni[m].code < code) lo = m + 1;
            else hi = m - 1;
        }
    }
    if (f->twobyte) return "";              /* a CID with no map means nothing */
    uint32_t u = code;
    if (code >= 0x80 && code < 0xa0) u = pdf_win1252[code - 0x80];
    if (u < 0x20 && u != '\t') return "";
    tmp[pdf_utf8(u, tmp)] = 0;
    return tmp;
}
static double pdf_width(PdfFont *f, uint32_t code) {
    if (f->twobyte) {
        for (int i = 0; i < f->ncw; i++)
            if (code >= f->cw[i].lo && code <= f->cw[i].hi) return f->cw[i].w;
        return f->dw;
    }
    int i = (int)code - f->firstchar;
    if (i >= 0 && i < f->nw && f->w[i] > 0) return f->w[i];
    return f->dw;
}
static void pdf_font_free(PdfFont *f) {
    free(f->uni); free(f->w); free(f->cw);
    memset(f, 0, sizeof *f);
}
static void pdf_font_load(Pdf *pdf, PSpan fd, PdfFont *f) {
    memset(f, 0, sizeof *f);
    f->dw = 500;
    if (!fd.p) return;

    PSpan sub = pdf_dget(fd, "Subtype");
    PSpan src = fd;                       /* where the widths live */
    if (pdf_isname(sub, "Type0")) {
        f->twobyte = 1;
        f->dw = 1000;
        PSpan df = pdf_dgetr(pdf, fd, "DescendantFonts");
        const char *p = pdf_arr_open(df);
        PSpan d0;
        if (p && pdf_arr_next(&p, df.e, &d0)) src = pdf_get(pdf, d0);
        f->dw = pdf_dnum(pdf, src, "DW", 1000);

        PSpan wa = pdf_dgetr(pdf, src, "W");     /* [c [w…] | c1 c2 w]* */
        const char *q = pdf_arr_open(wa);
        int cap = 0;
        PSpan v;
        while (q && pdf_arr_next(&q, wa.e, &v)) {
            uint32_t c1 = (uint32_t)pdf_num(v, 0);
            const char *save = q;
            PSpan nx;
            if (!pdf_arr_next(&q, wa.e, &nx)) break;
            const char *inner = pdf_arr_open(nx);
            if (inner) {
                PSpan wv;
                for (uint32_t c = c1; pdf_arr_next(&inner, nx.e, &wv); c++) {
                    if (f->ncw == cap) {
                        cap = cap ? cap * 2 : 64;
                        f->cw = xrealloc(f->cw, (size_t)cap * sizeof *f->cw);
                    }
                    f->cw[f->ncw].lo = f->cw[f->ncw].hi = c;
                    f->cw[f->ncw].w = pdf_num(wv, f->dw);
                    f->ncw++;
                }
            } else {
                uint32_t c2 = (uint32_t)pdf_num(nx, c1);
                PSpan wv;
                if (!pdf_arr_next(&q, wa.e, &wv)) { q = save; break; }
                if (c2 < c1 || c2 - c1 > 65535) c2 = c1;
                if (f->ncw == cap) {
                    cap = cap ? cap * 2 : 64;
                    f->cw = xrealloc(f->cw, (size_t)cap * sizeof *f->cw);
                }
                f->cw[f->ncw].lo = c1;
                f->cw[f->ncw].hi = c2;
                f->cw[f->ncw].w = pdf_num(wv, f->dw);
                f->ncw++;
            }
        }
    } else {
        f->firstchar = (int)pdf_dnum(pdf, fd, "FirstChar", 0);
        PSpan wa = pdf_dgetr(pdf, fd, "Widths");
        const char *q = pdf_arr_open(wa);
        int cap = 0;
        PSpan v;
        while (q && pdf_arr_next(&q, wa.e, &v)) {
            if (f->nw == cap) {
                cap = cap ? cap * 2 : 128;
                f->w = xrealloc(f->w, (size_t)cap * sizeof *f->w);
            }
            f->w[f->nw++] = pdf_num(v, 0);
        }
        PSpan desc = pdf_dgetr(pdf, fd, "FontDescriptor");
        if (desc.p) f->dw = pdf_dnum(pdf, desc, "MissingWidth", f->nw ? 0 : 500);
        if (f->dw <= 0 && !f->nw) f->dw = 500;
    }
    PSpan tu = pdf_dgetr(pdf, fd, "ToUnicode");
    if (tu.p) {
        size_t n;
        char *cm = pdf_stream(pdf, tu, &n);
        if (cm) { pdf_cmap(f, cm, cm + n); free(cm); }
    }
}

/* ── content streams ──────────────────────────────────────────────── */
/* `dir` is the quadrant the baseline advances in: 0 = +x, 1 = +y, 2 = -x,
 * 3 = -y. The layout rotates the page so the commonest one reads left to
 * right, which is what makes sideways scans and /Rotate'd pages legible. */
typedef struct { double x, y, h, w; char *s; int len; int dir; } PFrag;

typedef struct {
    PFrag  *f; int nf, fcap;
    struct { const char *key; char nm[64]; PdfFont f; } fc[24];
    int nfc;
} PdfOut;

static void mmul(const double *a, const double *b, double *r) {   /* r = a × b */
    double t[6];
    t[0] = a[0]*b[0] + a[1]*b[2];
    t[1] = a[0]*b[1] + a[1]*b[3];
    t[2] = a[2]*b[0] + a[3]*b[2];
    t[3] = a[2]*b[1] + a[3]*b[3];
    t[4] = a[4]*b[0] + a[5]*b[2] + b[4];
    t[5] = a[4]*b[1] + a[5]*b[3] + b[5];
    memcpy(r, t, sizeof t);
}
static double dabs(double v) { return v < 0 ? -v : v; }

static PdfFont *pdf_font_for(Pdf *pdf, PdfOut *o, PSpan res, const char *nm) {
    for (int i = 0; i < o->nfc; i++)
        if (o->fc[i].key == res.p && !strcmp(o->fc[i].nm, nm)) return &o->fc[i].f;
    int slot = o->nfc;
    if (slot == (int)(sizeof o->fc / sizeof *o->fc)) {   /* cache full: recycle */
        slot = 0;
        pdf_font_free(&o->fc[0].f);
    } else o->nfc++;
    o->fc[slot].key = res.p;
    snprintf(o->fc[slot].nm, sizeof o->fc[slot].nm, "%s", nm);
    PSpan fonts = pdf_dgetr(pdf, res, "Font");
    pdf_font_load(pdf, pdf_dgetr(pdf, fonts, nm), &o->fc[slot].f);
    return &o->fc[slot].f;
}
static void pdf_frag(PdfOut *o, double x, double y, double h, double w,
                     char *s, int len, int dir) {
    if (len <= 0 || o->nf >= PDF_MAXFRAG) { free(s); return; }
    if (o->nf == o->fcap) {
        o->fcap = o->fcap ? o->fcap * 2 : 512;
        o->f = xrealloc(o->f, (size_t)o->fcap * sizeof *o->f);
    }
    PFrag *g = &o->f[o->nf++];
    g->x = x; g->y = y; g->h = h; g->w = w; g->s = s; g->len = len; g->dir = dir;
}
/* Decode a literal (string) into bytes. */
static int pdf_litstr(const char *p, const char *e, unsigned char *out, int cap) {
    if (p >= e || *p != '(') return -1;
    p++;
    int n = 0, d = 1;
    while (p < e) {
        int c = (unsigned char)*p++;
        if (c == '\\') {
            if (p >= e) break;
            int x = (unsigned char)*p++;
            switch (x) {
                case 'n': c = '\n'; break;
                case 'r': c = '\r'; break;
                case 't': c = '\t'; break;
                case 'b': c = '\b'; break;
                case 'f': c = '\f'; break;
                case '\r': if (p < e && *p == '\n') p++;  continue;   /* line join */
                case '\n': continue;
                default:
                    if (x >= '0' && x <= '7') {
                        int v = x - '0';
                        for (int k = 0; k < 2 && p < e && *p >= '0' && *p <= '7'; k++)
                            v = v * 8 + (*p++ - '0');
                        c = v & 0xff;
                    } else c = x;
            }
        } else if (c == '(') { d++; }
        else if (c == ')') { if (--d == 0) break; }
        if (n < cap) out[n++] = (unsigned char)c;
    }
    return n;
}
/* Draw one string: append its glyphs and advance the text matrix. */
static void pdf_show(PdfOut *o, PdfFont *f, const unsigned char *s, int n,
                     double *tm, const double *ctm, double fs, double tc,
                     double tw, double th, double ts) {
    if (n <= 0) return;
    double trm[6] = { fs * th, 0, 0, fs, 0, ts }, m[6];
    mmul(trm, tm, m);
    mmul(m, ctm, m);
    double x0 = m[4], y0 = m[5];
    double scale = dabs(tm[0] * ctm[0]) + dabs(tm[1] * ctm[2]);
    if (scale < 1e-9) scale = 1;
    double h = fs * (dabs(tm[3] * ctm[3]) + dabs(tm[2] * ctm[1]));
    if (h < 0.01) h = fs;
    /* which way this baseline runs, from the transformed x-basis vector */
    int dir = (dabs(m[0]) >= dabs(m[1])) ? (m[0] >= 0 ? 0 : 2)
                                         : (m[1] >= 0 ? 1 : 3);

    char *buf = xmalloc((size_t)n * 5 + 8);
    int len = 0;
    double adv = 0;
    int step = f->twobyte ? 2 : 1;
    for (int i = 0; i + step <= n; i += step) {
        uint32_t code = f->twobyte ? (uint32_t)(s[i] << 8 | s[i + 1]) : s[i];
        char tmp[8];
        const char *g = pdf_glyph(f, code, tmp);
        for (const char *c = g; *c; c++) buf[len++] = *c;
        double w0 = pdf_width(f, code) / 1000.0;
        double a = (w0 * fs + tc + (code == 32 && step == 1 ? tw : 0)) * th;
        adv += a;
        double tr[6] = { 1, 0, 0, 1, a, 0 };
        mmul(tr, tm, tm);
    }
    buf[len] = 0;
    pdf_frag(o, x0, y0, h, adv * scale, buf, len, dir);
}
static void pdf_run(Pdf *pdf, const char *p, const char *e, PSpan res,
                    const double *ctm0, PdfOut *o, int depth);

/* Recurse into a form XObject so text placed inside one is not lost. */
static void pdf_do_form(Pdf *pdf, PdfOut *o, PSpan res, const char *nm,
                        const double *ctm, int depth) {
    PSpan xo = pdf_dgetr(pdf, pdf_dgetr(pdf, res, "XObject"), nm);
    if (!xo.p || !pdf_isname(pdf_dget(xo, "Subtype"), "Form")) return;
    size_t n;
    char *body = pdf_stream(pdf, xo, &n);
    if (!body) return;
    double m[6] = { 1, 0, 0, 1, 0, 0 };
    PSpan mx = pdf_dgetr(pdf, xo, "Matrix");
    const char *q = pdf_arr_open(mx);
    if (q) {
        PSpan v;
        for (int i = 0; i < 6 && pdf_arr_next(&q, mx.e, &v); i++) m[i] = pdf_num(v, m[i]);
    }
    mmul(m, ctm, m);
    PSpan r2 = pdf_dgetr(pdf, xo, "Resources");
    pdf_run(pdf, body, body + n, r2.p ? r2 : res, m, o, depth + 1);
    free(body);
}
/* Interpret a content stream. Only the text and coordinate operators matter;
 * everything else is skipped, but inline images have to be stepped over
 * explicitly or their binary payload would lex as operators. */
static void pdf_run(Pdf *pdf, const char *p, const char *e, PSpan res,
                    const double *ctm0, PdfOut *o, int depth) {
    if (depth > 8) return;
    double ctm[6];
    memcpy(ctm, ctm0, sizeof ctm);
    double gs[32][6];
    int ngs = 0;
    double tm[6] = { 1, 0, 0, 1, 0, 0 }, tlm[6] = { 1, 0, 0, 1, 0, 0 };
    double tc = 0, tw = 0, th = 1, tl = 0, ts = 0, fs = 0;
    PdfFont dummy;
    memset(&dummy, 0, sizeof dummy);
    dummy.dw = 500;
    PdfFont *font = &dummy;

    /* operand ring: the last few tokens seen before an operator */
    enum { NOP = 12 };
    PSpan ops[NOP];
    int nops = 0;

    while (p < e) {
        p = pdf_skip_ws(p, e);
        if (p >= e) break;
        int c = (unsigned char)*p;
        if (c == '/' || c == '(' || c == '<' || c == '[' || c == '{' ||
            isdigit(c) || c == '+' || c == '-' || c == '.') {
            const char *q = pdf_skip_obj(p, e);
            if (q <= p) break;
            if (nops == NOP) { memmove(ops, ops + 1, sizeof ops - sizeof *ops); nops--; }
            ops[nops].p = p;
            ops[nops].e = q;
            nops++;
            p = q;
            continue;
        }
        if (c == ']' || c == ')' || c == '>' || c == '}') { p++; continue; }

        const char *w = p;
        while (p < e && !pdf_ws((unsigned char)*p) && !pdf_delim((unsigned char)*p)) p++;
        size_t wl = (size_t)(p - w);
        if (wl == 0) { p++; continue; }
        #define OP(s) (wl == sizeof(s) - 1 && !memcmp(w, (s), sizeof(s) - 1))
        #define ARG(i) (nops > (i) ? ops[nops - 1 - (i)] : (PSpan){ NULL, NULL })

        if (OP("BT")) {
            double id[6] = { 1, 0, 0, 1, 0, 0 };
            memcpy(tm, id, sizeof tm);
            memcpy(tlm, id, sizeof tlm);
        } else if (OP("q")) {
            if (ngs < 32) memcpy(gs[ngs++], ctm, sizeof ctm);
        } else if (OP("Q")) {
            if (ngs > 0) memcpy(ctm, gs[--ngs], sizeof ctm);
        } else if (OP("cm") && nops >= 6) {
            double m[6];
            for (int i = 0; i < 6; i++) m[i] = pdf_num(ARG(5 - i), i == 0 || i == 3);
            mmul(m, ctm, ctm);
        } else if (OP("Tf") && nops >= 2) {
            char nm[64];
            fs = pdf_num(ARG(0), 0);
            if (pdf_name(ARG(1).p, ARG(1).e, nm, sizeof nm))
                font = pdf_font_for(pdf, o, res, nm);
        } else if (OP("Tc")) { tc = pdf_num(ARG(0), 0);
        } else if (OP("Tw")) { tw = pdf_num(ARG(0), 0);
        } else if (OP("Tz")) { th = pdf_num(ARG(0), 100) / 100.0;
        } else if (OP("TL")) { tl = pdf_num(ARG(0), 0);
        } else if (OP("Ts")) { ts = pdf_num(ARG(0), 0);
        } else if ((OP("Td") || OP("TD")) && nops >= 2) {
            double tx = pdf_num(ARG(1), 0), ty = pdf_num(ARG(0), 0);
            if (OP("TD")) tl = -ty;
            double m[6] = { 1, 0, 0, 1, tx, ty };
            mmul(m, tlm, tlm);
            memcpy(tm, tlm, sizeof tm);
        } else if (OP("Tm") && nops >= 6) {
            for (int i = 0; i < 6; i++) tlm[i] = pdf_num(ARG(5 - i), i == 0 || i == 3);
            memcpy(tm, tlm, sizeof tm);
        } else if (OP("T*") || OP("'") || OP("\"")) {
            if (OP("\"") && nops >= 3) { tw = pdf_num(ARG(2), tw); tc = pdf_num(ARG(1), tc); }
            double m[6] = { 1, 0, 0, 1, 0, -tl };
            mmul(m, tlm, tlm);
            memcpy(tm, tlm, sizeof tm);
            if (!OP("T*")) {
                unsigned char buf[4096];
                PSpan s = ARG(0);
                int n = s.p ? pdf_litstr(pdf_skip_ws(s.p, s.e), s.e, buf, sizeof buf) : -1;
                if (n > 0) pdf_show(o, font, buf, n, tm, ctm, fs, tc, tw, th, ts);
            }
        } else if (OP("Tj") && nops >= 1) {
            unsigned char buf[4096];
            PSpan s = ARG(0);
            const char *sp = pdf_skip_ws(s.p, s.e);
            int n = (sp && sp < s.e && *sp == '(') ? pdf_litstr(sp, s.e, buf, sizeof buf)
                                                   : pdf_hexstr(&sp, s.e, buf, sizeof buf);
            if (n > 0) pdf_show(o, font, buf, n, tm, ctm, fs, tc, tw, th, ts);
        } else if (OP("TJ") && nops >= 1) {
            PSpan a = ARG(0);
            const char *q = pdf_arr_open(a);
            PSpan v;
            while (q && pdf_arr_next(&q, a.e, &v)) {
                const char *sp = pdf_skip_ws(v.p, v.e);
                if (sp >= v.e) break;
                if (*sp == '(' || *sp == '<') {
                    unsigned char buf[4096];
                    int n = (*sp == '(') ? pdf_litstr(sp, v.e, buf, sizeof buf)
                                         : pdf_hexstr(&sp, v.e, buf, sizeof buf);
                    if (n > 0) pdf_show(o, font, buf, n, tm, ctm, fs, tc, tw, th, ts);
                } else {                                   /* kerning adjustment */
                    double adj = -pdf_num(v, 0) / 1000.0 * fs * th;
                    double m[6] = { 1, 0, 0, 1, adj, 0 };
                    mmul(m, tm, tm);
                }
            }
        } else if (OP("Do") && nops >= 1) {
            char nm[64];
            if (pdf_name(ARG(0).p, ARG(0).e, nm, sizeof nm))
                pdf_do_form(pdf, o, res, nm, ctm, depth);
        } else if (OP("BI")) {
            const char *q = p;                    /* skip to the matching EI */
            while (q + 2 <= e && !(q[0] == 'I' && q[1] == 'D' &&
                   (q + 2 == e || pdf_ws((unsigned char)q[2])))) q++;
            q = (q + 2 <= e) ? q + 3 : e;
            while (q + 2 <= e && !(pdf_ws((unsigned char)q[-1]) && q[0] == 'E' &&
                   q[1] == 'I' && (q + 2 == e || pdf_ws((unsigned char)q[2]) ||
                                   pdf_delim((unsigned char)q[2])))) q++;
            p = (q + 2 <= e) ? q + 2 : e;
        }
        #undef OP
        #undef ARG
        nops = 0;
    }
    pdf_font_free(&dummy);
}

/* ── layout ───────────────────────────────────────────────────────── */
static int pfrag_cmp(const void *a, const void *b) {
    const PFrag *x = a, *y = b;
    if (x->y != y->y) return x->y < y->y ? 1 : -1;     /* top of the page first */
    if (x->x != y->x) return x->x < y->x ? -1 : 1;
    return 0;
}
static int pfrag_xcmp(const void *a, const void *b) {
    const PFrag *x = a, *y = b;
    return x->x < y->x ? -1 : x->x > y->x;
}
static int dcmp(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}
static int pdf_nchars(const char *s, int len) {
    int n = 0;
    for (int i = 0; i < len; i++) if (((unsigned char)s[i] & 0xc0) != 0x80) n++;
    return n;
}
/* Fragments → text lines. Rows come from vertical proximity, columns from
 * dividing the horizontal offset by a typical character width, so tables and
 * indentation survive roughly intact. */
static void pdf_layout(PdfOut *o, Buf *b) {
    if (o->nf == 0) return;
    /* Turn the page so the dominant baseline direction reads left to right.
     * Weighting by character count keeps a sideways stamp or margin label
     * from out-voting the body text. */
    long vote[4] = { 0, 0, 0, 0 };
    for (int i = 0; i < o->nf; i++)
        vote[o->f[i].dir & 3] += pdf_nchars(o->f[i].s, o->f[i].len);
    int dom = 0;
    for (int d = 1; d < 4; d++) if (vote[d] > vote[dom]) dom = d;
    if (dom) {
        for (int i = 0; i < o->nf; i++) {
            double x = o->f[i].x, y = o->f[i].y;
            if (dom == 1)      { o->f[i].x = y;  o->f[i].y = -x; }
            else if (dom == 2) { o->f[i].x = -x; o->f[i].y = -y; }
            else               { o->f[i].x = -y; o->f[i].y = x;  }
        }
    }
    qsort(o->f, (size_t)o->nf, sizeof *o->f, pfrag_cmp);

    /* typical advance per character, as the column unit */
    double *samp = xmalloc((size_t)o->nf * sizeof *samp);
    int ns = 0;
    for (int i = 0; i < o->nf; i++) {
        int nc = pdf_nchars(o->f[i].s, o->f[i].len);
        if (nc > 0 && o->f[i].w > 0.01) samp[ns++] = o->f[i].w / nc;
    }
    double unit = 5.0;
    if (ns) {
        qsort(samp, (size_t)ns, sizeof *samp, dcmp);
        unit = samp[ns / 2];
    }
    free(samp);
    if (unit < 0.5) unit = 0.5;

    double left = o->f[0].x;
    for (int i = 1; i < o->nf; i++) if (o->f[i].x < left) left = o->f[i].x;

    char *line = xmalloc(PDF_MAXCOL + 8);
    double prev_y = 0, prev_h = 0;
    int first = 1;
    for (int i = 0; i < o->nf; ) {
        double rowy = o->f[i].y, rowh = o->f[i].h;
        int j = i + 1;
        for (; j < o->nf; j++) {
            double tol = 0.4 * (o->f[j].h > rowh ? o->f[j].h : rowh);
            if (tol < 1.0) tol = 1.0;
            if (tol > 14.0) tol = 14.0;
            if (rowy - o->f[j].y > tol) break;
            if (o->f[j].h > rowh) rowh = o->f[j].h;
        }
        qsort(o->f + i, (size_t)(j - i), sizeof *o->f, pfrag_xcmp);

        if (!first) {                    /* keep paragraph breaks visible */
            double gap = prev_y - rowy;
            double ref = (prev_h > rowh ? prev_h : rowh);
            if (ref > 0.01 && gap > ref * 1.7) {
                buf_insert_line(b, b->n, "", 0);
                if (gap > ref * 4.0) buf_insert_line(b, b->n, "", 0);
            }
        }
        int len = 0;
        double pen = left;               /* device x already written out */
        for (int k = i; k < j; k++) {
            PFrag *g = &o->f[k];
            int col = (int)((g->x - left) / unit + 0.5);
            if (col < 0) col = 0;
            if (col > len) {
                int pad = min2(col, PDF_MAXCOL - 1) - len;
                for (int s = 0; s < pad; s++) line[len++] = ' ';
            } else if (len > 0 && line[len - 1] != ' ' && g->x - pen > unit * 0.28) {
                if (len < PDF_MAXCOL) line[len++] = ' ';
            }
            for (int s = 0; s < g->len && len < PDF_MAXCOL; s++) {
                char ch = g->s[s];
                line[len++] = (ch == '\n' || ch == '\r' || ch == '\f') ? ' ' : ch;
            }
            pen = g->x + g->w;
        }
        while (len > 0 && line[len - 1] == ' ') len--;
        buf_insert_line(b, b->n, line, len);
        prev_y = rowy; prev_h = rowh; first = 0;
        i = j;
    }
    free(line);
}

/* ── page → buffer ────────────────────────────────────────────────── */
static void pdf_free(Pdf *p) {
    if (!p) return;
    for (int i = 0; i < p->nblob; i++) free(p->blob[i]);
    free(p->blob);
    free(p->obj);
    free(p->pg);
    free(p->raw);
    free(p);
}
/* Concatenate a page's /Contents (one stream, or an array of them). */
static char *pdf_contents(Pdf *pdf, PSpan page, size_t *outlen) {
    PSpan c = pdf_get(pdf, pdf_dget(page, "Contents"));
    char *all = NULL;
    size_t n = 0;
    const char *q = pdf_arr_open(c);
    PSpan v;
    for (int i = 0; ; i++) {
        PSpan item;
        if (q) {
            if (!pdf_arr_next(&q, c.e, &v)) break;
            item = pdf_get(pdf, v);
        } else {
            if (i > 0) break;
            item = c;
        }
        if (!item.p) continue;
        size_t sl;
        char *s = pdf_stream(pdf, item, &sl);
        if (!s) continue;
        char *t = realloc(all, n + sl + 2);
        if (!t) { free(s); break; }
        all = t;
        memcpy(all + n, s, sl);
        n += sl;
        all[n++] = '\n';                 /* streams split mid-token otherwise */
        free(s);
    }
    if (all) all[n] = 0;
    *outlen = n;
    return all;
}
static void pdf_page_into(Buf *b, int page) {
    Pdf *pdf = b->pdf;
    for (int i = 0; i < b->n; i++) free(b->ln[i].s);
    b->n = 0;
    b->cy = b->cx = b->rowoff = b->coloff = b->subrow = 0;
    b->sel = 0;
    b->hl_upto = 0;
    b->ver++;
    if (page < 0) page = 0;
    if (page >= pdf->npg) page = pdf->npg - 1;
    pdf->page = page;

    if (pdf->npg <= 0) {
        buf_insert_line(b, 0, "  no pages found in this PDF", 28);
        return;
    }
    PdfPage *g = &pdf->pg[page];
    size_t n;
    char *body = pdf_contents(pdf, g->dict, &n);
    PdfOut o;
    memset(&o, 0, sizeof o);
    if (body) {
        double ctm[6] = { 1, 0, 0, 1, 0, 0 };
        pdf_run(pdf, body, body + n, g->res, ctm, &o, 0);
        free(body);
    }
    pdf_layout(&o, b);
    for (int i = 0; i < o.nf; i++) free(o.f[i].s);
    free(o.f);
    for (int i = 0; i < o.nfc; i++) pdf_font_free(&o.fc[i].f);

    if (b->n == 0) {
        const char *m = pdf->encrypted
            ? "  this PDF is encrypted — sds cannot extract its text"
            : "  no extractable text on this page (it may be a scanned image)";
        buf_insert_line(b, 0, m, (int)strlen(m));
    }
    if (b->n == 0) buf_insert_line(b, 0, "", 0);
}
static Buf *pdf_load(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0 || sz > (400L << 20)) { fclose(f); return NULL; }
    rewind(f);
    char *raw = malloc((size_t)sz + 1);
    if (!raw) { fclose(f); return NULL; }
    size_t got = fread(raw, 1, (size_t)sz, f);
    fclose(f);
    raw[got] = 0;

    Pdf *pdf = calloc(1, sizeof *pdf);
    if (!pdf) { free(raw); return NULL; }
    pdf->raw = raw;
    pdf->rawlen = got;
    for (size_t i = 0; i + 8 <= got; i++)
        if (raw[i] == '/' && !memcmp(raw + i, "/Encrypt", 8)) { pdf->encrypted = 1; break; }
    pdf_index(pdf);
    pdf_pages(pdf);

    Buf *b = calloc(1, sizeof *b);
    if (!b) { pdf_free(pdf); return NULL; }
    snprintf(b->path, sizeof b->path, "%s", path);
    const char *slash = strrchr(path, '/');
    snprintf(b->name, sizeof b->name, "%s", slash ? slash + 1 : path);
    b->lang = LANG_TEXT;
    b->kind = TAB_PDF;
    b->pdf = pdf;
    pdf_page_into(b, 0);
    return b;
}
static int is_pdf_path(const char *path) {
    const char *dot = strrchr(path, '.');
    return dot && !strcasecmp(dot, ".pdf");
}

/* ── tabs ─────────────────────────────────────────────────────────── */
/* Point the focused pane at tab i. The one place `cur` is allowed to move. */
static void set_cur(int i) {
    if (i < 0 || i >= ntabs) return;
    cur = panes[curpane] = i;
}
/* Focus tab i, preferring a pane that already shows it over stealing the
 * focused one — otherwise Ctrl+P on a file you can already see would yank it
 * out from under the pane it lives in. */
static void focus_tab(int i) {
    if (i < 0 || i >= ntabs) return;
    for (int p = 0; p < npanes; p++)
        if (panes[p] == i) { curpane = p; cur = i; return; }
    set_cur(i);
}
static void open_file(const char *path) {
    for (int i = 0; i < ntabs; i++)
        if (tabs[i]->kind != TAB_TERM && strcmp(tabs[i]->path, path) == 0) {
            focus_tab(i);
            return;
        }
    if (ntabs == MAX_TABS) { set_msg("too many open tabs", NULL); return; }
    Buf *b = is_pdf_path(path) ? pdf_load(path) : buf_load(path);
    if (!b) { set_msg("can't open %s", path); return; }
    if (b->kind != TAB_PDF) b->kind = TAB_FILE;
    tabs[ntabs++] = b;
    set_cur(ntabs - 1);
}
static void close_tab(int i) {
    buf_free(tabs[i]);
    memmove(tabs + i, tabs + i + 1, (size_t)(ntabs - i - 1) * sizeof(Buf *));
    ntabs--;
    /* keep every pane pointing at the tab it was showing; a pane whose tab
     * just went away falls to whatever slid into its place */
    for (int p = 0; p < MAX_PANES; p++) {
        if (panes[p] > i) panes[p]--;
        else if (panes[p] == i) panes[p] = min2(i, ntabs - 1);
    }
    if (ntabs == 0) {
        npanes = 1; curpane = 0;
        for (int p = 0; p < MAX_PANES; p++) panes[p] = -1;
        cur = -1;
        return;
    }
    /* That fallback can land a pane on a tab another pane already shows.
     * Fold the duplicate away instead of rendering one buffer twice — they
     * would share a cursor and scroll position, which reads as a glitch. */
    for (int p = npanes - 1; p > 0; p--)
        for (int q = 0; q < p; q++)
            if (panes[p] == panes[q]) {
                memmove(panes + p, panes + p + 1,
                        (size_t)(npanes - p - 1) * sizeof *panes);
                panes[--npanes] = -1;
                if (curpane > p)       curpane--;
                else if (curpane == p) curpane = q;
                break;
            }
    if (curpane >= npanes) curpane = npanes - 1;
    cur = panes[curpane];
}
static void pane_close(void) {
    if (npanes < 2) { set_msg("only one pane open", NULL); return; }
    memmove(panes + curpane, panes + curpane + 1,
            (size_t)(npanes - curpane - 1) * sizeof *panes);
    panes[--npanes] = -1;
    if (curpane >= npanes) curpane = npanes - 1;
    cur = panes[curpane];
}
/* Alt+Shift+N: put tab N in a pane. Already visible elsewhere → focus it;
 * already in *this* pane → fold the pane away, so the same chord toggles. */
static void pane_show_tab(int t) {
    if (t < 0 || t >= ntabs) return;
    for (int p = 0; p < npanes; p++)
        if (panes[p] == t) {
            if (p == curpane) { pane_close(); return; }
            curpane = p; cur = t;
            return;
        }
    if (npanes < MAX_PANES) { panes[npanes] = t; curpane = npanes++; }
    else panes[curpane] = t;
    cur = panes[curpane];
}
/* A terminal cannot send "Shift+3" — it sends '#'. Undo that so Alt+Shift+N
 * can address the tab numbers printed in the tab bar. Returns 0-9, or -1. */
static int shift_digit(int c) {
    static const char sh[] = ")!@#$%^&*(";
    int ch = c - 3000;                             /* the ALT() offset */
    if (ch <= 0 || ch > 255) return -1;
    const char *p = strchr(sh, ch);
    return p ? (int)(p - sh) : -1;
}

/* ── file tree ────────────────────────────────────────────────────── */
static Node *node_new(const char *name, const char *path, int is_dir, Node *parent) {
    Node *n = calloc(1, sizeof *n);
    if (!n) die("out of memory");
    n->name = xstrdup(name); n->path = xstrdup(path);
    n->is_dir = is_dir; n->parent = parent;
    n->depth = parent ? parent->depth + 1 : -1;
    return n;
}
static int node_cmp(const void *a, const void *b) {
    const Node *x = *(Node * const *)a, *y = *(Node * const *)b;
    if (x->is_dir != y->is_dir) return y->is_dir - x->is_dir;
    return strcasecmp(x->name, y->name);
}
static void node_load(Node *d) {
    if (d->loaded) return;
    d->loaded = 1;
    DIR *dp = opendir(d->path);
    if (!dp) return;
    struct dirent *e;
    while ((e = readdir(dp))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char p[PATH_MAX];
        if (snprintf(p, sizeof p, "%s/%s", d->path, e->d_name) >= (int)sizeof p)
            continue;
        struct stat st;
        if (stat(p, &st) != 0) continue;
        Node *k = node_new(e->d_name, p, S_ISDIR(st.st_mode), d);
        d->kid = xrealloc(d->kid, (size_t)(d->nkid + 1) * sizeof(Node *));
        d->kid[d->nkid++] = k;
    }
    closedir(dp);
    qsort(d->kid, (size_t)d->nkid, sizeof(Node *), node_cmp);
}
static void node_free(Node *n) {
    for (int i = 0; i < n->nkid; i++) node_free(n->kid[i]);
    free(n->kid); free(n->name); free(n->path);
    free(n);
}
/* Re-read `d` from disk, reusing nodes that are still there so expanded
 * folders stay expanded. Unloaded folders are left alone — they'll read
 * fresh whenever they're first expanded. */
static void node_refresh(Node *d) {
    if (!d->loaded) return;
    Node **old = d->kid;
    int nold = d->nkid;
    d->kid = NULL;
    d->nkid = 0;

    DIR *dp = opendir(d->path);
    if (!dp) {                       /* folder vanished under us */
        for (int i = 0; i < nold; i++) node_free(old[i]);
        free(old);
        d->loaded = 0;
        return;
    }
    struct dirent *e;
    while ((e = readdir(dp))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char p[PATH_MAX];
        if (snprintf(p, sizeof p, "%s/%s", d->path, e->d_name) >= (int)sizeof p)
            continue;
        struct stat st;
        if (stat(p, &st) != 0) continue;
        int is_dir = S_ISDIR(st.st_mode);

        Node *k = NULL;
        for (int i = 0; i < nold; i++)          /* reuse a surviving node */
            if (old[i] && old[i]->is_dir == is_dir &&
                !strcmp(old[i]->name, e->d_name)) {
                k = old[i];
                old[i] = NULL;
                break;
            }
        if (!k) k = node_new(e->d_name, p, is_dir, d);
        else if (k->is_dir) node_refresh(k);    /* recurse into loaded dirs */

        d->kid = xrealloc(d->kid, (size_t)(d->nkid + 1) * sizeof(Node *));
        d->kid[d->nkid++] = k;
    }
    closedir(dp);
    for (int i = 0; i < nold; i++) if (old[i]) node_free(old[i]);  /* gone */
    free(old);
    qsort(d->kid, (size_t)d->nkid, sizeof(Node *), node_cmp);
}
static void flatten(Node *d) {
    for (int i = 0; i < d->nkid; i++) {
        Node *k = d->kid[i];
        if (nvis == viscap) {
            viscap = viscap ? viscap * 2 : 128;
            vis = xrealloc(vis, (size_t)viscap * sizeof(Node *));
        }
        vis[nvis++] = k;
        if (k->is_dir && k->expanded) flatten(k);
    }
}
static void tree_rebuild(void) {
    nvis = 0;
    flatten(root);
    if (tsel >= nvis) tsel = nvis - 1;
    if (tsel < 0) tsel = 0;
}
static void tree_open_selected(void) {
    if (nvis == 0) return;
    Node *n = vis[tsel];
    if (n->is_dir) {
        n->expanded = !n->expanded;
        if (n->expanded) node_load(n);
        tree_rebuild();
    } else open_file(n->path);
}
static void tree_toggle(void) {
    tree_hidden = !tree_hidden;
    set_msg(tree_hidden ? "sidebar hidden" : "sidebar shown", NULL);
}
/* Collapse one level. Collapsing when there is nothing left to collapse —
 * a top-level entry, already folded — folds the sidebar itself away, so
 * repeated Alt+Left in the root directory reclaims the whole width. */
static void tree_collapse(void) {
    if (tree_hidden) return;
    if (nvis == 0) { tree_hidden = 1; return; }
    Node *n = vis[tsel];
    if (n->is_dir && n->expanded) { n->expanded = 0; tree_rebuild(); return; }
    if (n->parent && n->parent != root) {
        for (int i = 0; i < nvis; i++)
            if (vis[i] == n->parent) { tsel = i; break; }
        return;
    }
    tree_hidden = 1;
    set_msg("sidebar hidden — Alt+Right or Alt+B to bring it back", NULL);
}
static void git_refresh(void);
/* rescan the whole tree, keeping the cursor on the same path if it survived */
static void tree_refresh(void) {
    git_refresh();
    char keep[PATH_MAX] = "";
    if (nvis) snprintf(keep, sizeof keep, "%s", vis[tsel]->path);
    node_refresh(root);
    tree_rebuild();
    if (keep[0])
        for (int i = 0; i < nvis; i++)
            if (!strcmp(vis[i]->path, keep)) { tsel = i; break; }
    if (tsel >= nvis) tsel = max2(0, nvis - 1);
}
static void tree_expand(void) {
    if (tree_hidden) { tree_hidden = 0; return; }   /* first Alt+Right un-hides */
    if (nvis == 0) return;
    Node *n = vis[tsel];
    if (n->is_dir && !n->expanded) { n->expanded = 1; node_load(n); tree_rebuild(); }
}

/* ── lexer ────────────────────────────────────────────────────────── */
/* The keyword lists are written as readable space-delimited strings, but
 * scanning them with strstr for every identifier on every visible line got
 * expensive as the lists grew. Split them once into sorted arrays and binary
 * search instead, so adding keywords stays free. */
typedef struct { const char *w; int len; } Kw;
static struct { Kw *kw; int nkw; Kw *ty; int nty; } kwidx[NLANGS];
static int kw_cmp(const void *a, const void *b) {
    const Kw *x = a, *y = b;
    int n = x->len < y->len ? x->len : y->len;
    int c = memcmp(x->w, y->w, (size_t)n);
    if (c) return c;
    return x->len - y->len;
}
static void kw_split(const char *src, Kw **out, int *nout) {
    int n = 0, cap = 16;
    Kw *v = xmalloc((size_t)cap * sizeof *v);
    const char *p = src;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        const char *s = p;
        while (*p && *p != ' ') p++;
        if (n == cap) { cap *= 2; v = xrealloc(v, (size_t)cap * sizeof *v); }
        v[n].w = s;
        v[n].len = (int)(p - s);
        n++;
    }
    qsort(v, (size_t)n, sizeof *v, kw_cmp);
    *out = v; *nout = n;
}
static void kw_index_build(void) {
    for (int i = 0; i < NLANGS; i++) {
        kw_split(langs[i].kw,    &kwidx[i].kw, &kwidx[i].nkw);
        kw_split(langs[i].types, &kwidx[i].ty, &kwidx[i].nty);
    }
}
static int kw_class(const Lang *lg, const char *s, int len) {
    char buf[64];
    if (len > 63) return HA_DEF;
    /* nocase languages (SQL) keep their tables lowercase, so fold the token */
    if (lg->nocase) {
        for (int i = 0; i < len; i++) buf[i] = (char)tolower((unsigned char)s[i]);
        s = buf;
    }
    int li = (int)(lg - langs);
    if (li < 0 || li >= NLANGS) return HA_DEF;
    Kw key = { s, len };
    if (kwidx[li].nkw && bsearch(&key, kwidx[li].kw, (size_t)kwidx[li].nkw,
                                 sizeof(Kw), kw_cmp)) return HA_KW;
    if (kwidx[li].nty && bsearch(&key, kwidx[li].ty, (size_t)kwidx[li].nty,
                                 sizeof(Kw), kw_cmp)) return HA_TYPE;
    return HA_DEF;
}
static int tok_at(const char *s, int len, int i, const char *tok) {
    int tl = (int)strlen(tok);
    return tl && i + tl <= len && memcmp(s + i, tok, (size_t)tl) == 0;
}
/* lex one line; fills attr[0..len) if attr != NULL; returns end state */
static int lex_line(const Lang *lg, const char *s, int len, int st,
                    unsigned char *attr) {
#define SETA(i, a) do { if (attr) attr[i] = (unsigned char)(a); } while (0)
    int i = 0;
    if (attr) memset(attr, HA_DEF, (size_t)len);
    /* resume a multi-line construct */
    while (i < len && st != ST_NORM) {
        const char *end = st == ST_BCOM ? lg->bc : st == ST_TRI1 ? lg->t1 : lg->t2;
        int a = st == ST_BCOM ? HA_COM : HA_STR;
        if (tok_at(s, len, i, end)) {
            for (int k = 0; k < (int)strlen(end); k++) SETA(i + k, a);
            i += (int)strlen(end);
            st = ST_NORM;
        } else { SETA(i, a); i++; }
    }
    if (st != ST_NORM) return st;   /* whole line consumed */
    /* preprocessor line */
    if (lg->preproc) {
        int j = 0;
        while (j < len && isspace((unsigned char)s[j])) j++;
        if (j < len && s[j] == '#') {
            for (int k = j; k < len; k++) SETA(k, HA_PRE);
            return ST_NORM;
        }
    }
    while (i < len) {
        unsigned char c = (unsigned char)s[i];
        /* line comments */
        if (tok_at(s, len, i, lg->lc) || tok_at(s, len, i, lg->lc2)) {
            for (int k = i; k < len; k++) SETA(k, HA_COM);
            return ST_NORM;
        }
        /* block comment open */
        if (tok_at(s, len, i, lg->bo)) {
            int bol = (int)strlen(lg->bo);
            for (int k = 0; k < bol; k++) SETA(i + k, HA_COM);
            i += bol;
            st = ST_BCOM;
            while (i < len) {
                if (tok_at(s, len, i, lg->bc)) {
                    int bcl = (int)strlen(lg->bc);
                    for (int k = 0; k < bcl; k++) SETA(i + k, HA_COM);
                    i += bcl; st = ST_NORM; break;
                }
                SETA(i, HA_COM); i++;
            }
            if (st == ST_BCOM) return ST_BCOM;
            continue;
        }
        /* triple-quoted strings (python) */
        if (tok_at(s, len, i, lg->t1) || tok_at(s, len, i, lg->t2)) {
            int one = tok_at(s, len, i, lg->t1);
            const char *d = one ? lg->t1 : lg->t2;
            int dl = (int)strlen(d);
            for (int k = 0; k < dl; k++) SETA(i + k, HA_STR);
            i += dl;
            st = one ? ST_TRI1 : ST_TRI2;
            while (i < len) {
                if (tok_at(s, len, i, d)) {
                    for (int k = 0; k < dl; k++) SETA(i + k, HA_STR);
                    i += dl; st = ST_NORM; break;
                }
                SETA(i, HA_STR); i++;
            }
            if (st != ST_NORM) return st;
            continue;
        }
        /* strings */
        if (c == '"' || (c == '`' && lg->bq) || (c == '\'' && lg->sq == 2)) {
            char q = (char)c;
            SETA(i, HA_STR); i++;
            while (i < len) {
                SETA(i, HA_STR);
                if (s[i] == '\\' && i + 1 < len) { SETA(i + 1, HA_STR); i += 2; continue; }
                if (s[i] == q) { i++; break; }
                i++;
            }
            continue;
        }
        /* char literals: 'x' or '\x' */
        if (c == '\'' && lg->sq == 1) {
            int close = -1;
            if (i + 2 < len && s[i+1] == '\\' && s[i+3] == '\'') close = i + 3;
            else if (i + 2 < len && s[i+2] == '\'') close = i + 2;
            if (close > 0) {
                for (int k = i; k <= close; k++) SETA(k, HA_STR);
                i = close + 1;
            } else i++;                 /* lifetime / apostrophe */
            continue;
        }
        /* numbers */
        if (isdigit(c)) {
            int j = i;
            while (j < len && (isalnum((unsigned char)s[j]) || s[j] == '.' || s[j] == '_'))
                j++;
            for (int k = i; k < j; k++) SETA(k, HA_NUM);
            i = j;
            continue;
        }
        /* identifiers / keywords */
        if (word_ch(c) && !isdigit(c)) {
            int j = i;
            while (j < len && word_ch((unsigned char)s[j])) j++;
            int cls = kw_class(lg, s + i, j - i);
            for (int k = i; k < j; k++) SETA(k, cls);
            i = j;
            continue;
        }
        i++;
    }
    return ST_NORM;
#undef SETA
}
/* make hst valid for lines [0..upto] */
static void ensure_hl(Buf *b, int upto) {
    if (upto >= b->n) upto = b->n - 1;
    if (b->hl_upto == 0) b->ln[0].hst = ST_NORM;
    for (int i = b->hl_upto; i < upto; i++)
        b->ln[i + 1].hst = lex_line(b->lang, b->ln[i].s, b->ln[i].len,
                                    b->ln[i].hst, NULL);
    if (upto > b->hl_upto) b->hl_upto = upto;
}

/* ── tree-sitter (optional) ───────────────────────────────────────── */
/* Built only with -DSDS_TREESITTER. Grammars are dlopen'd at runtime rather
 * than linked, so sds keeps working when a language's grammar is missing —
 * it just falls back to the keyword lexer above. Nothing here is required
 * for sds to build or run.
 *
 *   grammars: $XDG_DATA_HOME/sds/grammars/libtree-sitter-<lang>.so, then
 *             the usual system library directories
 *   queries:  $XDG_DATA_HOME/sds/queries/<lang>/highlights.scm
 *
 * `sds --fetch-grammar <lang>` builds and installs both.                  */
#ifdef SDS_TREESITTER
typedef struct {
    char         name[24];
    const TSLanguage *lang;
    TSQuery     *query;
    unsigned char *cap;        /* query capture index → HA_* */
    int          tried;        /* load attempted; don't retry every keystroke */
} TSGram;

static TSGram tsgram[NLANGS];

/* sds language name → tree-sitter grammar name (mostly identity) */
static const char *ts_name_of(const Lang *lg) {
    if (!strcmp(lg->name, "c++"))  return "cpp";
    if (!strcmp(lg->name, "make")) return "make";
    if (!strcmp(lg->name, "text")) return NULL;
    return lg->name;
}
/* tree-sitter capture names are dotted and open-ended ("keyword.coroutine",
 * "type.builtin", …); classify on the leading component. */
static unsigned char ts_cap_attr(const char *nm, uint32_t len) {
    char b[64];
    uint32_t n = len < sizeof b - 1 ? len : (uint32_t)sizeof b - 1;
    memcpy(b, nm, n); b[n] = 0;
    char *dot = strchr(b, '.');
    if (dot) *dot = 0;
    if (!strcmp(b, "keyword"))                          return HA_KW;
    if (!strcmp(b, "type") || !strcmp(b, "constructor")) return HA_TYPE;
    if (!strcmp(b, "function") || !strcmp(b, "method")) return HA_TYPE;
    if (!strcmp(b, "string") || !strcmp(b, "character")) return HA_STR;
    if (!strcmp(b, "comment"))                          return HA_COM;
    if (!strcmp(b, "number") || !strcmp(b, "float"))    return HA_NUM;
    if (!strcmp(b, "preproc") || !strcmp(b, "keyword_directive")) return HA_PRE;
    if (!strcmp(b, "constant")) {
        return (dot && !strcmp(dot + 1, "numeric")) ? HA_NUM : HA_TYPE;
    }
    return HA_DEF;
}
static void ts_data_dir(char *out, size_t cap, const char *sub) {
    const char *xdg = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
    if (xdg && *xdg) snprintf(out, cap, "%s/sds/%s", xdg, sub);
    else if (home && *home) snprintf(out, cap, "%s/.local/share/sds/%s", home, sub);
    else snprintf(out, cap, "./%s", sub);
}
static char *slurp(const char *path, uint32_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    if (n < 0 || n > (1 << 22)) { fclose(f); return NULL; }
    rewind(f);
    char *s = xmalloc((size_t)n + 1);
    size_t got = fread(s, 1, (size_t)n, f);
    fclose(f);
    s[got] = 0;
    *len = (uint32_t)got;
    return s;
}
/* A grammar's highlight query is usually only the part that differs from a
 * base language: tree-sitter-cpp's file has no rules for `int` or `char8_t`
 * because those live in tree-sitter-c's. Upstream expresses that with a
 * "; inherits: c" comment (which not every repo actually includes), so honor
 * the directive and also fall back to a known parent per language. */
static const char *ts_parent_of(const char *nm) {
    if (!strcmp(nm, "cpp"))        return "c";
    if (!strcmp(nm, "typescript")) return "javascript";
    if (!strcmp(nm, "tsx"))        return "typescript";
    return NULL;
}
/* Read <nm>'s query, prefixed by whatever it inherits. Depth-capped so a
 * malformed inherits cycle cannot recurse forever. */
static char *ts_query_src(const char *nm, uint32_t *outlen, int depth) {
    char dir[PATH_MAX - 64], path[PATH_MAX];
    ts_data_dir(dir, sizeof dir, "queries");
    snprintf(path, sizeof path, "%s/%s/highlights.scm", dir, nm);
    uint32_t len = 0;
    char *own = slurp(path, &len);
    if (!own) return NULL;
    if (depth >= 4) { *outlen = len; return own; }

    /* explicit "; inherits: a,b" on one of the first lines */
    char parents[128] = "";
    const char *ih = strstr(own, "inherits:");
    if (ih && ih - own < 200) {
        ih += 9;
        while (*ih == ' ') ih++;
        size_t k = 0;
        while (*ih && *ih != '\n' && k + 1 < sizeof parents) parents[k++] = *ih++;
        parents[k] = 0;
    }
    if (!parents[0]) {
        const char *p = ts_parent_of(nm);
        if (p) snprintf(parents, sizeof parents, "%s", p);
    }
    if (!parents[0]) { *outlen = len; return own; }

    /* concatenate each parent's query ahead of this one */
    char *acc = NULL;
    uint32_t acclen = 0;
    char *save = NULL;
    for (char *tok = strtok_r(parents, ", \t", &save); tok;
         tok = strtok_r(NULL, ", \t", &save)) {
        if (!strcmp(tok, nm)) continue;
        uint32_t plen = 0;
        char *ps = ts_query_src(tok, &plen, depth + 1);
        if (!ps) continue;
        acc = xrealloc(acc, acclen + plen + 2);
        memcpy(acc + acclen, ps, plen);
        acclen += plen;
        acc[acclen++] = '\n';
        free(ps);
    }
    if (!acc) { *outlen = len; return own; }
    acc = xrealloc(acc, acclen + len + 1);
    memcpy(acc + acclen, own, len);
    acclen += len;
    acc[acclen] = 0;
    free(own);
    *outlen = acclen;
    return acc;
}
/* Load grammar + highlight query for `lg`, once. Returns NULL if either is
 * unavailable, which simply means this buffer keeps using the lexer. */
static TSGram *ts_for(const Lang *lg) {
    int li = (int)(lg - langs);
    if (li < 0 || li >= NLANGS) return NULL;
    TSGram *g = &tsgram[li];
    if (g->tried) return g->lang && g->query ? g : NULL;
    g->tried = 1;

    const char *nm = ts_name_of(lg);
    if (!nm) return NULL;
    snprintf(g->name, sizeof g->name, "%s", nm);

    /* the directory is bounded well below PATH_MAX so the composed paths
     * below always fit — sized explicitly to keep -Wformat-truncation quiet */
    char dir[PATH_MAX - 64], sym[64];
    ts_data_dir(dir, sizeof dir, "grammars");
    const char *cands[4];
    char c0[PATH_MAX], c1[64 + 24], c2[64 + 24];
    snprintf(c0, sizeof c0, "%s/libtree-sitter-%s.so", dir, nm);
    snprintf(c1, sizeof c1, "libtree-sitter-%s.so", nm);        /* ld.so path */
    snprintf(c2, sizeof c2, "/usr/lib/libtree-sitter-%s.so", nm);
    cands[0] = c0; cands[1] = c1; cands[2] = c2; cands[3] = NULL;

    void *dl = NULL;
    for (int i = 0; cands[i] && !dl; i++) dl = dlopen(cands[i], RTLD_LAZY | RTLD_LOCAL);
    if (!dl) return NULL;

    snprintf(sym, sizeof sym, "tree_sitter_%s", nm);
    const TSLanguage *(*fn)(void) = (const TSLanguage *(*)(void))
        (uintptr_t)dlsym(dl, sym);
    if (!fn) { dlclose(dl); return NULL; }
    g->lang = fn();
    if (!g->lang) { dlclose(dl); return NULL; }

    uint32_t qlen = 0;
    char *src = ts_query_src(nm, &qlen, 0);
    if (!src) { g->lang = NULL; dlclose(dl); return NULL; }

    uint32_t erroff; TSQueryError errtype;
    g->query = ts_query_new(g->lang, src, qlen, &erroff, &errtype);
    free(src);
    if (!g->query) {
        /* An inherited query can reference node types this grammar does not
         * have, which fails the whole compile. Retry with just this
         * language's own rules before giving up on tree-sitter entirely. */
        char dir[PATH_MAX - 64], path[PATH_MAX];
        ts_data_dir(dir, sizeof dir, "queries");
        snprintf(path, sizeof path, "%s/%s/highlights.scm", dir, nm);
        src = slurp(path, &qlen);
        if (src) {
            g->query = ts_query_new(g->lang, src, qlen, &erroff, &errtype);
            free(src);
        }
    }
    if (!g->query) { g->lang = NULL; dlclose(dl); return NULL; }

    uint32_t nc = ts_query_capture_count(g->query);
    g->cap = xmalloc(nc ? nc : 1);
    for (uint32_t i = 0; i < nc; i++) {
        uint32_t l;
        const char *cn = ts_query_capture_name_for_id(g->query, i, &l);
        g->cap[i] = ts_cap_attr(cn, l);
    }
    return g;
}

/* byte offset of the start of each line, rebuilt when the buffer changes */
static void ts_offsets(Buf *b) {
    if (!b->ts_off_dirty && b->ts_off) return;
    b->ts_off = xrealloc(b->ts_off, (size_t)(b->n + 1) * sizeof *b->ts_off);
    uint32_t o = 0;
    for (int i = 0; i < b->n; i++) { b->ts_off[i] = o; o += (uint32_t)b->ln[i].len + 1; }
    b->ts_off[b->n] = o;
    b->ts_bytes = o;
    b->ts_off_dirty = 0;
}
/* feed the line array to tree-sitter without flattening it into one string */
static const char *ts_read(void *payload, uint32_t byte, TSPoint pt, uint32_t *len) {
    (void)pt;
    Buf *b = payload;
    if (byte >= b->ts_bytes) { *len = 0; return ""; }
    int lo = 0, hi = b->n - 1, li = 0;
    while (lo <= hi) {                       /* which line holds `byte`? */
        int mid = (lo + hi) / 2;
        if (b->ts_off[mid] <= byte) { li = mid; lo = mid + 1; } else hi = mid - 1;
    }
    uint32_t within = byte - b->ts_off[li];
    if (within < (uint32_t)b->ln[li].len) {
        *len = (uint32_t)b->ln[li].len - within;
        return b->ln[li].s + within;
    }
    *len = 1;                                 /* the line's newline */
    return "\n";
}
static void ts_reparse(Buf *b) {
    TSGram *g = ts_for(b->lang);
    if (!g) return;
    if (!b->ts_parser) {
        b->ts_parser = ts_parser_new();
        if (!ts_parser_set_language(b->ts_parser, g->lang)) {
            ts_parser_delete(b->ts_parser);
            b->ts_parser = NULL;
            return;
        }
    }
    if (b->ts_tree && b->ts_ver == b->ver) return;      /* still current */
    ts_offsets(b);
    TSInput in = { b, ts_read, TSInputEncodingUTF8, NULL };
    /* ts_note_edit() has already applied every edit to the old tree, so this
     * reparse is incremental: cost tracks the size of the change, not the file */
    TSTree *t = ts_parser_parse(b->ts_parser, b->ts_tree, in);
    if (!t) return;
    if (b->ts_tree) ts_tree_delete(b->ts_tree);
    b->ts_tree = t;
    b->ts_ver = b->ver;
}
/* Collected captures for the visible window, shortest-span-last so that the
 * most specific capture is the one that ends up painted. */
typedef struct { uint32_t s, e; unsigned char a; } TSSpan;
static TSSpan *tsspan = NULL;
static int     ntsspan = 0, tsspan_cap = 0;
static int     tsspan_buf_ver = -1;
static Buf    *tsspan_buf = NULL;
static int     tsspan_y0 = -1, tsspan_y1 = -1;

/* a buffer is going away — make sure the span cache stops referring to it */
static void ts_forget(Buf *b) {
    if (tsspan_buf == b) { tsspan_buf = NULL; ntsspan = 0; tsspan_buf_ver = -1; }
}
static int tsspan_cmp(const void *x, const void *y) {
    const TSSpan *a = x, *b = y;
    uint32_t la = a->e - a->s, lb = b->e - b->s;
    if (la != lb) return la < lb ? 1 : -1;      /* longest first */
    return a->s < b->s ? -1 : a->s > b->s;
}
/* Run the highlight query over lines [y0,y1] only. */
static void ts_collect(Buf *b, int y0, int y1) {
    TSGram *g = ts_for(b->lang);
    if (!g) return;
    ts_reparse(b);
    if (!b->ts_tree) return;
    if (tsspan_buf == b && tsspan_buf_ver == b->ts_ver &&
        tsspan_y0 == y0 && tsspan_y1 == y1) return;         /* cache hit */

    ntsspan = 0;
    tsspan_buf = b; tsspan_buf_ver = b->ts_ver;
    tsspan_y0 = y0; tsspan_y1 = y1;

    static TSQueryCursor *cur_q = NULL;
    if (!cur_q) cur_q = ts_query_cursor_new();
    ts_query_cursor_set_byte_range(cur_q, b->ts_off[y0], b->ts_off[y1 + 1]);
    ts_query_cursor_exec(cur_q, g->query, ts_tree_root_node(b->ts_tree));

    TSQueryMatch m;
    while (ts_query_cursor_next_match(cur_q, &m)) {
        for (uint16_t i = 0; i < m.capture_count; i++) {
            unsigned char a = g->cap[m.captures[i].index];
            if (a == HA_DEF) continue;
            uint32_t s = ts_node_start_byte(m.captures[i].node);
            uint32_t e = ts_node_end_byte(m.captures[i].node);
            if (e <= s) continue;
            if (ntsspan == tsspan_cap) {
                tsspan_cap = tsspan_cap ? tsspan_cap * 2 : 256;
                tsspan = xrealloc(tsspan, (size_t)tsspan_cap * sizeof *tsspan);
            }
            tsspan[ntsspan].s = s; tsspan[ntsspan].e = e; tsspan[ntsspan].a = a;
            ntsspan++;
        }
    }
    qsort(tsspan, (size_t)ntsspan, sizeof *tsspan, tsspan_cmp);
}
/* Paint line `li`'s attrs from the collected spans. Returns 0 if tree-sitter
 * has nothing for this buffer and the caller should use the lexer. */
static int ts_line_attrs(Buf *b, int li, unsigned char *attr) {
    if (!b->ts_tree || !tsspan || tsspan_buf != b) return 0;
    int len = b->ln[li].len;
    memset(attr, HA_DEF, (size_t)len);
    uint32_t ls = b->ts_off[li], le = ls + (uint32_t)len;
    for (int i = 0; i < ntsspan; i++) {
        if (tsspan[i].e <= ls || tsspan[i].s >= le) continue;
        uint32_t a = tsspan[i].s > ls ? tsspan[i].s - ls : 0;
        uint32_t z = tsspan[i].e < le ? tsspan[i].e - ls : (uint32_t)len;
        for (uint32_t k = a; k < z; k++) attr[k] = tsspan[i].a;
    }
    return 1;
}
#endif /* SDS_TREESITTER */

/* One line's syntax attributes, from tree-sitter when a grammar is installed
 * for this language and from the built-in lexer otherwise. */
static void hl_line(Buf *b, int li, unsigned char *attr) {
#ifdef SDS_TREESITTER
    if (ts_line_attrs(b, li, attr)) return;
#endif
    Line *l = &b->ln[li];
    lex_line(b->lang, l->s, l->len, l->hst, attr);
}

/* ── color pairs ──────────────────────────────────────────────────── */
enum { CP_TAB_ACT = 1, CP_TAB, CP_SEL, CP_DIR, CP_STATUS, CP_LINENO, CP_MUTED,
       CP_KW, CP_TYPE, CP_STR, CP_COM, CP_NUM, CP_PRE, CP_FIND, CP_ERR };
enum { OV_SEL = 1, OV_FIND = 2, OV_BRK = 4 };

/* ── themes ───────────────────────────────────────────────────────── */
/* A theme is twelve role colors. Each carries an explicit basic-8 fallback so
 * the built-in themes still look right on an 8-color terminal — "tux" in
 * particular is defined to reproduce sds's original hardcoded palette exactly. */
typedef struct { int rgb; short basic; } Col;

typedef struct {
    char name[32];
    Col  accent, bg, fg, muted, bg_alt, error;   /* UI roles   */
    Col  kw, type, str, com, num, pre;           /* syntax     */
} Theme;

/* Tux chrome, VS Code syntax.
 *
 * The UI is the penguin: a black body, a white belly, and the amber beak
 * (#f5a623) for the accent — tabs, tree selection, line numbers. Carrying
 * that amber into the syntax colors as well turned the code itself too
 * yellow, so those follow VS Code's default dark theme instead, which is
 * both familiar and well separated by hue. */
static const Theme theme_tux = {
    "tux",
    { 0xf5a623, COLOR_YELLOW }, { 0x000000, COLOR_BLACK },   /* accent, bg    */
    { 0xffffff, COLOR_WHITE  }, { 0x5f5f5f, COLOR_BLUE  },   /* fg,     muted */
    { 0x141414, COLOR_BLACK  }, { 0xe0503a, COLOR_RED   },   /* bg_alt, error */
    { 0x569cd6, COLOR_BLUE   }, { 0x4ec9b0, COLOR_CYAN  },   /* kw,     type  */
    { 0xce9178, COLOR_RED    }, { 0x6a9955, COLOR_GREEN },   /* str,    com   */
    { 0xb5cea8, COLOR_YELLOW }, { 0xc586c0, COLOR_MAGENTA }, /* num,    pre   */
};

/* Presets carried over from the Python-era themes.toml. Their syntax colors
 * are derived from each palette rather than invented. */
static const Theme theme_presets[] = {
    /* sds's pre-theme look: the eight basic terminal colors, kept so the
     * original appearance is still one config line away. */
    { "classic",
      { 0xf5a623, COLOR_YELLOW  }, { 0x000000, COLOR_BLACK },
      { 0xffffff, COLOR_WHITE   }, { 0x3465a4, COLOR_BLUE  },
      { 0x1a1a1a, COLOR_BLACK   }, { 0xd23c3d, COLOR_RED   },
      { 0xad7fa8, COLOR_MAGENTA }, { 0x34e2e2, COLOR_CYAN  },
      { 0x8ae234, COLOR_GREEN   }, { 0x3465a4, COLOR_BLUE  },
      { 0xef2929, COLOR_RED     }, { 0x34e2e2, COLOR_CYAN  } },
    { "monokai",
      { 0xfd971f, COLOR_YELLOW  }, { 0x272822, COLOR_BLACK },
      { 0xf8f8f2, COLOR_WHITE   }, { 0x75715e, COLOR_BLUE  },
      { 0x3e3d32, COLOR_BLACK   }, { 0xf92672, COLOR_RED   },
      { 0xf92672, COLOR_MAGENTA }, { 0x66d9ef, COLOR_CYAN  },
      { 0xe6db74, COLOR_GREEN   }, { 0x75715e, COLOR_BLUE  },
      { 0xae81ff, COLOR_RED     }, { 0xa6e22e, COLOR_CYAN  } },
    { "dracula",
      { 0xbd93f9, COLOR_YELLOW  }, { 0x282a36, COLOR_BLACK },
      { 0xf8f8f2, COLOR_WHITE   }, { 0x6272a4, COLOR_BLUE  },
      { 0x44475a, COLOR_BLACK   }, { 0xff5555, COLOR_RED   },
      { 0xff79c6, COLOR_MAGENTA }, { 0x8be9fd, COLOR_CYAN  },
      { 0xf1fa8c, COLOR_GREEN   }, { 0x6272a4, COLOR_BLUE  },
      { 0xbd93f9, COLOR_RED     }, { 0x50fa7b, COLOR_CYAN  } },
    { "nord",
      { 0x88c0d0, COLOR_YELLOW  }, { 0x2e3440, COLOR_BLACK },
      { 0xd8dee9, COLOR_WHITE   }, { 0x4c566a, COLOR_BLUE  },
      { 0x3b4252, COLOR_BLACK   }, { 0xbf616a, COLOR_RED   },
      { 0x81a1c1, COLOR_MAGENTA }, { 0x8fbcbb, COLOR_CYAN  },
      { 0xa3be8c, COLOR_GREEN   }, { 0x616e88, COLOR_BLUE  },
      { 0xb48ead, COLOR_RED     }, { 0x5e81ac, COLOR_CYAN  } },
    { "gruvbox",
      { 0xfe8019, COLOR_YELLOW  }, { 0x282828, COLOR_BLACK },
      { 0xebdbb2, COLOR_WHITE   }, { 0x928374, COLOR_BLUE  },
      { 0x3c3836, COLOR_BLACK   }, { 0xfb4934, COLOR_RED   },
      { 0xfb4934, COLOR_MAGENTA }, { 0x8ec07c, COLOR_CYAN  },
      { 0xb8bb26, COLOR_GREEN   }, { 0x928374, COLOR_BLUE  },
      { 0xd3869b, COLOR_RED     }, { 0xfabd2f, COLOR_CYAN  } },
};

static Theme theme;                 /* the active theme */

/* nearest xterm-256 index for an rgb triple: try the 6×6×6 cube and the
 * 24-step grey ramp, keep whichever is closer */
static int rgb_to_256(int rgb) {
    int r = rgb >> 16 & 0xff, g = rgb >> 8 & 0xff, b = rgb & 0xff;
    static const int lv[6] = { 0, 95, 135, 175, 215, 255 };
    int ci[3], comp[3] = { r, g, b };
    for (int k = 0; k < 3; k++) {
        int best = 0, bd = 1 << 30;
        for (int i = 0; i < 6; i++) {
            int d = abs(lv[i] - comp[k]);
            if (d < bd) { bd = d; best = i; }
        }
        ci[k] = best;
    }
    int cube = 16 + 36 * ci[0] + 6 * ci[1] + ci[2];
    int cd = 0;
    for (int k = 0; k < 3; k++) { int d = lv[ci[k]] - comp[k]; cd += d * d; }

    int grey = (r * 299 + g * 587 + b * 114) / 1000;
    int gi = (grey - 8) / 10;
    if (gi < 0) gi = 0;
    if (gi > 23) gi = 23;
    int gv = 8 + gi * 10, gd = 0;
    for (int k = 0; k < 3; k++) { int d = gv - comp[k]; gd += d * d; }

    return gd < cd ? 232 + gi : cube;
}
/* Exact colors.
 *
 * A 256-color terminal can only show the fixed xterm palette, so theme colors
 * normally snap to the nearest entry (#f5a623 lands on #ffaf00). Most modern
 * terminals can however be told what a palette slot *means*, which terminfo
 * reports as `ccc` and ncurses as can_change_color(). When that is available
 * sds redefines a handful of slots, taken from the top of the palette, to the
 * theme's exact RGB — and puts them back on exit so the shell's colors are
 * not left altered.
 *
 * Slots are shared with whatever runs in a terminal tab, which is why they
 * come from the top (the light end of the greyscale ramp, rarely referenced
 * by name) and why `true_color = off` exists in the config.              */
#define TC_SLOTS 16
static int   tc_want = 1;               /* config: 1 = use exact colors */
static int   tc_on   = 0;               /* resolved at startup */
static int   tc_rgb[TC_SLOTS];          /* rgb held by each allocated slot */
static short tc_idx[TC_SLOTS];
static int   tc_n = 0;

/* The canonical xterm-256 value of a palette index: 16-231 are the 6×6×6
 * cube, 232-255 the greyscale ramp. Restoring has to be computed like this
 * rather than read back with color_content(), which only knows the first
 * few entries and reports nonsense above them — using it would leave the
 * palette worse than it started. */
static int xterm256_rgb(int i) {
    static const int lv[6] = { 0, 95, 135, 175, 215, 255 };
    if (i >= 232 && i <= 255) { int v = 8 + (i - 232) * 10; return (v << 16) | (v << 8) | v; }
    if (i >= 16 && i <= 231) {
        i -= 16;
        return (lv[i / 36] << 16) | (lv[(i / 6) % 6] << 8) | lv[i % 6];
    }
    return 0;
}
/* 0-255 to curses' 0-1000, rounded so the value survives the round trip:
 * terminfo's initc converts back with a truncating (v*255)/1000, so picking
 * the smallest scaled value that floors to v is what lands exactly on v.
 * Truncating here instead loses one unit per channel. */
static short tc_scale(int v) {
    int x = (v * 1000 + 254) / 255;         /* ceil */
    return (short)(x > 1000 ? 1000 : x);
}
static void tc_set(short idx, int rgb) {
    init_color(idx, tc_scale(rgb >> 16 & 0xff),
                    tc_scale(rgb >> 8  & 0xff),
                    tc_scale(rgb       & 0xff));
}
static int tc_alloc(int rgb) {
    for (int i = 0; i < tc_n; i++) if (tc_rgb[i] == rgb) return tc_idx[i];
    if (tc_n >= TC_SLOTS) return rgb_to_256(rgb);      /* out of slots */
    short idx = (short)(COLORS - 1 - tc_n);
    if (idx < 16 || idx > 255) return rgb_to_256(rgb);
    tc_set(idx, rgb);
    tc_rgb[tc_n] = rgb;
    tc_idx[tc_n] = idx;
    tc_n++;
    return idx;
}
/* hand the palette back the way a 256-color terminal defines it */
static void tc_restore(void) {
    for (int i = 0; i < tc_n; i++) tc_set(tc_idx[i], xterm256_rgb(tc_idx[i]));
    tc_n = 0;
}
/* map a theme color onto whatever this terminal can actually show */
static int col_of(Col c) {
    if (tc_on)         return tc_alloc(c.rgb);
    if (COLORS >= 256) return rgb_to_256(c.rgb);
    return c.basic;
}
static void apply_theme(void) {
    if (!has_colors()) return;
    tc_restore();                       /* re-theming reuses the same slots */
    tc_on = tc_want && can_change_color() && COLORS >= 256;
    int bg = col_of(theme.bg), fg = col_of(theme.fg);
    /* -1 keeps the terminal's own background, which is what sds did before and
     * what makes transparent terminals look right */
    int dfl = (theme.bg.rgb == 0x000000) ? -1 : bg;
    init_pair(CP_TAB_ACT, bg,                  col_of(theme.accent));
    init_pair(CP_TAB,     fg,                  col_of(theme.bg_alt));
    init_pair(CP_SEL,     bg,                  col_of(theme.accent));
    init_pair(CP_DIR,     col_of(theme.type),  dfl);
    init_pair(CP_STATUS,  bg,                  fg);
    init_pair(CP_LINENO,  col_of(theme.accent), dfl);
    init_pair(CP_MUTED,   col_of(theme.muted), dfl);
    init_pair(CP_KW,      col_of(theme.kw),    dfl);
    init_pair(CP_TYPE,    col_of(theme.type),  dfl);
    init_pair(CP_STR,     col_of(theme.str),   dfl);
    init_pair(CP_COM,     col_of(theme.com),   dfl);
    init_pair(CP_NUM,     col_of(theme.num),   dfl);
    init_pair(CP_PRE,     col_of(theme.pre),   dfl);
    init_pair(CP_FIND,    bg,                  col_of(theme.str));
    init_pair(CP_ERR,     fg,                  col_of(theme.error));
}

/* ── keybindings ──────────────────────────────────────────────────── */
enum { KB_QUIT, KB_SAVE, KB_CLOSE_TAB, KB_HELP, KB_RUN, KB_TERM, KB_FIND,
       KB_FIND_NEXT, KB_REPLACE, KB_GOTO, KB_QUICKOPEN, KB_COMPLETE,
       KB_NEW_ENTRY, KB_DEL_ENTRY, KB_REFRESH, KB_TREE_UP, KB_TREE_DOWN,
       KB_TREE_COLLAPSE, KB_TREE_EXPAND, KB_TREE_OPEN, KB_TAB_PREV,
       KB_TAB_NEXT, KB_WRAP, KB_SIDEBAR, KB_MOVE_UP, KB_MOVE_DOWN,
       KB_PANE_LEFT, KB_PANE_RIGHT, KB_PANE_UP, KB_PANE_DOWN, KB_PANE_CLOSE,
       KB_N };

static const struct { const char *name; int dflt; } kb_def[] = {
    { "quit",          ALT('q')          }, { "save",        CTRL('s')         },
    { "close_tab",     ALT('w')          }, { "help",        ALT('h')          },
    { "run",           ALT('r')          }, { "terminal",    ALT('t')          },
    { "find",          CTRL('f')         }, { "find_next",   KEY_F(3)          },
    { "replace",       CTRL('r')         }, { "goto",        CTRL('g')         },
    { "quick_open",    CTRL('p')         }, { "complete",    0 /* Ctrl+Space */},
    { "new_entry",     K_AINS            }, { "delete_entry", K_ADEL           },
    { "refresh",       KEY_F(5)          }, { "tree_up",     MK(3, D_UP)       },
    { "tree_down",     MK(3, D_DOWN)     }, { "tree_collapse", MK(3, D_LEFT)   },
    { "tree_expand",   MK(3, D_RIGHT)    }, { "tree_open",   ALT('\n')         },
    { "tab_prev",      ALT(',')          }, { "tab_next",    ALT('.')          },
    { "wrap",          ALT('z')          }, { "sidebar",     ALT('b')          },
    { "move_line_up",  MK(6, D_UP)       }, { "move_line_down", MK(6, D_DOWN)  },
    { "pane_left",     MK(4, D_LEFT)     }, { "pane_right",  MK(4, D_RIGHT)    },
    { "pane_up",       MK(4, D_UP)       }, { "pane_down",   MK(4, D_DOWN)     },
    { "pane_close",    ALT(')')          },
};
static int kb[KB_N];

/* "ctrl+shift+left", "alt+q", "f5", "escape" … → an sds key code, or -1 */
static int parse_key(const char *s) {
    int alt = 0, ctrl = 0, shift = 0;
    char buf[64];
    snprintf(buf, sizeof buf, "%s", s);
    for (char *p = buf; *p; p++) *p = (char)tolower((unsigned char)*p);

    char *k = buf;
    for (;;) {
        char *plus = strchr(k, '+');
        if (!plus) break;
        *plus = 0;
        if      (!strcmp(k, "ctrl") || !strcmp(k, "control")) ctrl = 1;
        else if (!strcmp(k, "alt")  || !strcmp(k, "meta"))    alt = 1;
        else if (!strcmp(k, "shift"))                         shift = 1;
        else return -1;
        k = plus + 1;
    }
    if (!*k) return -1;

    /* arrows / home / end take the modifier-encoding path */
    int dir = -1;
    if      (!strcmp(k, "up"))    dir = D_UP;
    else if (!strcmp(k, "down"))  dir = D_DOWN;
    else if (!strcmp(k, "left"))  dir = D_LEFT;
    else if (!strcmp(k, "right")) dir = D_RIGHT;
    else if (!strcmp(k, "home"))  dir = D_HOME;
    else if (!strcmp(k, "end"))   dir = D_END;
    if (dir >= 0) {
        int mod = 1 + shift + 2 * alt + 4 * ctrl;      /* xterm's scheme */
        if (mod == 1) {
            switch (dir) {
                case D_UP:    return KEY_UP;
                case D_DOWN:  return KEY_DOWN;
                case D_LEFT:  return KEY_LEFT;
                case D_RIGHT: return KEY_RIGHT;
                case D_HOME:  return KEY_HOME;
                default:      return KEY_END;
            }
        }
        return MK(mod, dir);
    }
    if (k[0] == 'f' && isdigit((unsigned char)k[1])) {
        int n = atoi(k + 1);
        if (n >= 1 && n <= 12) return KEY_F(n);
        return -1;
    }
    if (!strcmp(k, "insert")) return alt ? K_AINS : KEY_IC;
    if (!strcmp(k, "delete")) return alt ? K_ADEL : KEY_DC;
    if (!strcmp(k, "enter") || !strcmp(k, "return"))
        return alt ? ALT('\n') : '\r';
    if (!strcmp(k, "escape") || !strcmp(k, "esc")) return alt ? ALT(27) : 27;
    if (!strcmp(k, "space"))  return ctrl ? 0 : (alt ? ALT(' ') : ' ');
    if (!strcmp(k, "tab"))    return '\t';
    if (!strcmp(k, "pageup"))   return KEY_PPAGE;
    if (!strcmp(k, "pagedown")) return KEY_NPAGE;
    if (k[1]) return -1;                          /* multi-char, unrecognised */
    /* A terminal has no way to say "shift+3": it just sends '#'. Spell the
     * shifted digits out so "alt+shift+3" in the config means what it looks
     * like, which is what the pane bindings are written as. */
    if (shift && isdigit((unsigned char)k[0])) k[0] = ")!@#$%^&*("[k[0] - '0'];
    if (ctrl) return CTRL(k[0]);
    if (alt)  return ALT(k[0]);
    return (unsigned char)k[0];
}

/* ── config ───────────────────────────────────────────────────────── */
static char cfg_dir[PATH_MAX];
static char cfg_warn[256] = "";     /* shown once in the status bar at startup */

static void cfg_dir_init(void) {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    if (xdg && *xdg) snprintf(cfg_dir, sizeof cfg_dir, "%s/sds", xdg);
    else if (home && *home) snprintf(cfg_dir, sizeof cfg_dir, "%s/.config/sds", home);
    else cfg_dir[0] = 0;
}
/* strip a trailing comment and surrounding whitespace/quotes, in place */
static char *cfg_clean(char *s) {
    int inq = 0;
    for (char *p = s; *p; p++) {
        if (*p == '"') inq = !inq;
        else if ((*p == '#' || *p == ';') && !inq) { *p = 0; break; }
    }
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) e--;
    *e = 0;
    if (e > s + 1 && *s == '"' && e[-1] == '"') { s++; e[-1] = 0; }
    return s;
}
static int parse_hex(const char *s, int *out) {
    if (*s == '#') s++;
    if (strlen(s) != 6) return 0;
    char *end;
    long v = strtol(s, &end, 16);
    if (*end) return 0;
    *out = (int)v;
    return 1;
}
/* assign one "key = value" into a theme; returns 1 if the key was a color */
static int theme_set(Theme *t, const char *k, const char *v) {
    static const char *names[] = { "accent", "bg", "fg", "muted", "bg_alt",
                                   "error", "kw", "type", "str", "com",
                                   "num", "pre" };
    Col *slots[] = { &t->accent, &t->bg, &t->fg, &t->muted, &t->bg_alt,
                     &t->error, &t->kw, &t->type, &t->str, &t->com,
                     &t->num, &t->pre };
    for (size_t i = 0; i < sizeof names / sizeof *names; i++)
        if (!strcmp(k, names[i])) {
            int rgb;
            if (!parse_hex(v, &rgb)) return 1;      /* claimed, but unusable */
            slots[i]->rgb = rgb;
            /* a file-supplied color has no declared 8-color fallback; pick the
             * nearest basic color so 8-color terminals still differentiate */
            static const int b8[8] = { 0x000000, 0xcc0000, 0x4e9a06, 0xc4a000,
                                       0x3465a4, 0x75507b, 0x06989a, 0xd3d7cf };
            int best = 0, bd = 1 << 30;
            for (int c = 0; c < 8; c++) {
                int dr = (b8[c] >> 16 & 0xff) - (rgb >> 16 & 0xff);
                int dg = (b8[c] >> 8  & 0xff) - (rgb >> 8  & 0xff);
                int db = (b8[c]       & 0xff) - (rgb       & 0xff);
                int d = dr * dr + dg * dg + db * db;
                if (d < bd) { bd = d; best = c; }
            }
            slots[i]->basic = (short)best;
            return 1;
        }
    return 0;
}
/* Scan a themes file for [want] and load it into `out`. Returns 1 if found. */
static int themes_file_load(const char *path, const char *want, Theme *out) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[512];
    int in = 0, found = 0;
    while (fgets(line, sizeof line, f)) {
        char *s = cfg_clean(line);
        if (!*s) continue;
        if (*s == '[') {
            char *e = strchr(s, ']');
            if (!e) continue;
            *e = 0;
            in = !strcmp(s + 1, want);
            if (in) { found = 1; snprintf(out->name, sizeof out->name, "%s", want); }
            continue;
        }
        if (!in) continue;
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        theme_set(out, cfg_clean(s), cfg_clean(eq + 1));
    }
    fclose(f);
    return found;
}

static const char *DEFAULT_CONFIG =
"# SimpleDevSuite configuration.\n"
"#\n"
"# Every key below is optional — anything you leave out (or delete) falls back\n"
"# to the built-in default shown here. Unknown keys are ignored rather than\n"
"# treated as errors. Nothing hot-reloads; restart sds after editing.\n"
"\n"
"# Color theme. \"tux\" is black/white/amber after the penguin. \"classic\" is\n"
"# sds's original eight-color look. Also built in: monokai, dracula, nord,\n"
"# gruvbox. Any [name] table in the `themes` file next to this one also\n"
"# works, and overrides a built-in of the same name.\n"
"theme = \"tux\"\n"
"\n"
"# Show the theme's exact colors instead of snapping them to the terminal's\n"
"# fixed 256-color palette. Needs a terminal that allows redefining palette\n"
"# entries (terminfo `ccc`); sds detects that and quietly falls back if not.\n"
"# It borrows a few slots from the top of the palette and restores them on\n"
"# exit. Set to off if colors inside terminal tabs look wrong.\n"
"true_color = on\n"
"\n"
"[editor]\n"
"tab_width = 4        # render width of a tab character (1-16)\n"
"soft_wrap = false    # start with word wrap on (toggle at runtime with Alt+Z)\n"
"\n"
"[tree]\n"
"width = 30           # sidebar width in columns\n"
"# Below this terminal width the sidebar hides itself so the editor stays\n"
"# usable. Collapsing past the top level (Alt+Left) also hides it; Alt+Right\n"
"# or Alt+B brings it back. Set to 0 to never auto-hide.\n"
"auto_hide_below = 80\n"
"\n"
"[keys]\n"
"# Syntax: lowercase, \"+\"-joined, e.g. \"ctrl+s\", \"alt+shift+up\", \"f5\".\n"
"# Only these app-level actions are remappable; in-editor chords are not.\n"
"#\n"
"# The tree keys default to Alt+<something>. If your window manager grabs Alt\n"
"# (common on i3/sway/Hyprland), remap them to combos it doesn't intercept.\n"
"quit          = \"alt+q\"\n"
"save          = \"ctrl+s\"\n"
"close_tab     = \"alt+w\"\n"
"help          = \"alt+h\"\n"
"run           = \"alt+r\"\n"
"terminal      = \"alt+t\"\n"
"find          = \"ctrl+f\"\n"
"find_next     = \"f3\"\n"
"replace       = \"ctrl+r\"\n"
"goto          = \"ctrl+g\"\n"
"quick_open    = \"ctrl+p\"\n"
"complete      = \"ctrl+space\"\n"
"new_entry     = \"alt+insert\"\n"
"delete_entry  = \"alt+delete\"\n"
"refresh       = \"f5\"\n"
"tree_up       = \"alt+up\"\n"
"tree_down     = \"alt+down\"\n"
"tree_collapse = \"alt+left\"\n"
"tree_expand   = \"alt+right\"\n"
"tree_open     = \"alt+enter\"\n"
"tab_prev      = \"alt+,\"\n"
"tab_next      = \"alt+.\"\n"
"wrap          = \"alt+z\"\n"
"sidebar       = \"alt+b\"\n"
"\n"
"# Split view. Alt+Shift+1..9 puts that tab in its own pane (up to four, in a\n"
"# 2x2 grid); pressing it again on the pane you are in folds that pane away.\n"
"# The arrows move focus between panes geometrically.\n"
"pane_left     = \"alt+shift+left\"\n"
"pane_right    = \"alt+shift+right\"\n"
"pane_up       = \"alt+shift+up\"\n"
"pane_down     = \"alt+shift+down\"\n"
"pane_close    = \"alt+shift+0\"\n"
"\n"
"# Moving lines lived on Alt+Shift+Up/Down before the panes took those over.\n"
"move_line_up   = \"ctrl+shift+up\"\n"
"move_line_down = \"ctrl+shift+down\"\n";

static const char *DEFAULT_THEMES =
"# SimpleDevSuite themes.\n"
"#\n"
"# Select one from your `config` with e.g. theme = \"monokai\". A [name] table\n"
"# here overrides a built-in theme of the same name, so you can retune \"tux\"\n"
"# without renaming it. Add your own by copying a block and changing the name.\n"
"#\n"
"# Twelve roles, all \"#rrggbb\". Missing ones inherit the built-in default.\n"
"#   accent  active tab, tree selection, line numbers\n"
"#   bg      primary background (and the text color on accent surfaces)\n"
"#   fg      primary text\n"
"#   muted   tree rules, dim text, the inactive UI\n"
"#   bg_alt  tab bar background\n"
"#   error   error dialogs and messages\n"
"#   kw type str com num pre    syntax: keywords, types, strings,\n"
"#                              comments, numbers, preprocessor\n"
"\n"
"# Tux chrome with VS Code dark syntax: the UI is the penguin (black body,\n"
"# white belly, amber beak), while the code itself uses VS Code's default\n"
"# dark colors, which separate better by hue than an all-amber palette.\n"
"[tux]\n"
"accent = \"#f5a623\"  # the beak — tabs, tree selection, line numbers\n"
"bg     = \"#000000\"  # the body\n"
"fg     = \"#ffffff\"  # the belly\n"
"muted  = \"#5f5f5f\"\n"
"bg_alt = \"#141414\"\n"
"error  = \"#e0503a\"\n"
"kw     = \"#569cd6\"  # VS Code blue\n"
"type   = \"#4ec9b0\"  # teal\n"
"str    = \"#ce9178\"  # salmon\n"
"com    = \"#6a9955\"  # green\n"
"num    = \"#b5cea8\"  # pale green\n"
"pre    = \"#c586c0\"  # purple\n"
"\n"
"# sds's original look, on the eight basic terminal colors.\n"
"[classic]\n"
"accent = \"#f5a623\"\n"
"bg     = \"#000000\"\n"
"fg     = \"#ffffff\"\n"
"muted  = \"#3465a4\"\n"
"bg_alt = \"#1a1a1a\"\n"
"error  = \"#d23c3d\"\n"
"kw     = \"#ad7fa8\"\n"
"type   = \"#34e2e2\"\n"
"str    = \"#8ae234\"\n"
"com    = \"#3465a4\"\n"
"num    = \"#ef2929\"\n"
"pre    = \"#34e2e2\"\n"
"\n"
"[monokai]\n"
"accent = \"#fd971f\"\n"
"bg     = \"#272822\"\n"
"fg     = \"#f8f8f2\"\n"
"muted  = \"#75715e\"\n"
"bg_alt = \"#3e3d32\"\n"
"error  = \"#f92672\"\n"
"kw     = \"#f92672\"\n"
"type   = \"#66d9ef\"\n"
"str    = \"#e6db74\"\n"
"com    = \"#75715e\"\n"
"num    = \"#ae81ff\"\n"
"pre    = \"#a6e22e\"\n"
"\n"
"[dracula]\n"
"accent = \"#bd93f9\"\n"
"bg     = \"#282a36\"\n"
"fg     = \"#f8f8f2\"\n"
"muted  = \"#6272a4\"\n"
"bg_alt = \"#44475a\"\n"
"error  = \"#ff5555\"\n"
"kw     = \"#ff79c6\"\n"
"type   = \"#8be9fd\"\n"
"str    = \"#f1fa8c\"\n"
"com    = \"#6272a4\"\n"
"num    = \"#bd93f9\"\n"
"pre    = \"#50fa7b\"\n"
"\n"
"[nord]\n"
"accent = \"#88c0d0\"\n"
"bg     = \"#2e3440\"\n"
"fg     = \"#d8dee9\"\n"
"muted  = \"#4c566a\"\n"
"bg_alt = \"#3b4252\"\n"
"error  = \"#bf616a\"\n"
"kw     = \"#81a1c1\"\n"
"type   = \"#8fbcbb\"\n"
"str    = \"#a3be8c\"\n"
"com    = \"#616e88\"\n"
"num    = \"#b48ead\"\n"
"pre    = \"#5e81ac\"\n"
"\n"
"[gruvbox]\n"
"accent = \"#fe8019\"\n"
"bg     = \"#282828\"\n"
"fg     = \"#ebdbb2\"\n"
"muted  = \"#928374\"\n"
"bg_alt = \"#3c3836\"\n"
"error  = \"#fb4934\"\n"
"kw     = \"#fb4934\"\n"
"type   = \"#8ec07c\"\n"
"str    = \"#b8bb26\"\n"
"com    = \"#928374\"\n"
"num    = \"#d3869b\"\n"
"pre    = \"#fabd2f\"\n";

static int write_if_absent(const char *path, const char *body) {
    if (access(path, F_OK) == 0) return 0;
    FILE *f = fopen(path, "w");
    if (!f) return 0;
    fputs(body, f);
    fclose(f);
    return 1;
}
/* Create ~/.config/sds/{config,themes,syntax/} the first time sds runs.
 * Best-effort: a read-only home just means the built-in defaults apply. */
static int cfg_write_defaults(void) {
    if (!cfg_dir[0]) return 0;
    char p[PATH_MAX];
    const char *home = getenv("HOME");
    if (home && *home) {                        /* ensure ~/.config exists */
        snprintf(p, sizeof p, "%s/.config", home);
        mkdir(p, 0755);
    }
    if (mkdir(cfg_dir, 0755) != 0 && errno != EEXIST) return 0;
    snprintf(p, sizeof p, "%s/syntax", cfg_dir);
    mkdir(p, 0755);
    int wrote = 0;
    snprintf(p, sizeof p, "%s/config", cfg_dir);
    wrote |= write_if_absent(p, DEFAULT_CONFIG);
    snprintf(p, sizeof p, "%s/themes", cfg_dir);
    wrote |= write_if_absent(p, DEFAULT_THEMES);
    return wrote;
}
/* Read the config, resolve the theme, and fill kb[]. Never fails hard: a
 * malformed file leaves the built-in defaults in place and sets cfg_warn. */
static void cfg_load(void) {
    char want[32] = "tux";
    for (int i = 0; i < KB_N; i++) kb[i] = kb_def[i].dflt;

    cfg_dir_init();
    int fresh = cfg_write_defaults();

    char path[PATH_MAX];
    if (cfg_dir[0]) {
        snprintf(path, sizeof path, "%s/config", cfg_dir);
        FILE *f = fopen(path, "r");
        if (f) {
            char line[512];
            char sect[32] = "";
            int bad = 0;
            while (fgets(line, sizeof line, f)) {
                char *s = cfg_clean(line);
                if (!*s) continue;
                if (*s == '[') {
                    char *e = strchr(s, ']');
                    if (!e) { bad++; continue; }
                    *e = 0;
                    snprintf(sect, sizeof sect, "%s", s + 1);
                    continue;
                }
                char *eq = strchr(s, '=');
                if (!eq) { bad++; continue; }
                *eq = 0;
                char *k = cfg_clean(s), *v = cfg_clean(eq + 1);

                if (!sect[0] && !strcmp(k, "theme")) {
                    snprintf(want, sizeof want, "%s", v);
                } else if (!sect[0] && !strcmp(k, "true_color")) {
                    tc_want = !(!strcmp(v, "false") || !strcmp(v, "off") ||
                                !strcmp(v, "0"));
                } else if (!strcmp(sect, "editor")) {
                    if (!strcmp(k, "tab_width")) {
                        int n = atoi(v);
                        if (n >= 1 && n <= TABSTOP_MAX) tabstop = n;
                    } else if (!strcmp(k, "soft_wrap")) {
                        wrap = !strcmp(v, "true") || !strcmp(v, "1");
                    }
                } else if (!strcmp(sect, "tree")) {
                    if (!strcmp(k, "width")) {
                        int n = atoi(v);
                        if (n >= 10 && n <= 100) tree_w = n;
                    } else if (!strcmp(k, "auto_hide_below")) {
                        tree_autohide = atoi(v);
                    }
                } else if (!strcmp(sect, "keys")) {
                    for (int i = 0; i < KB_N; i++)
                        if (!strcmp(k, kb_def[i].name)) {
                            int c = parse_key(v);
                            if (c >= 0) kb[i] = c;
                            else bad++;
                            break;
                        }
                }
            }
            fclose(f);
            if (bad)
                snprintf(cfg_warn, sizeof cfg_warn,
                         "config: %d unusable line(s), using defaults there", bad);
        }
    }

    /* built-in theme first, then let a themes file of the same name override */
    theme = theme_tux;
    for (size_t i = 0; i < sizeof theme_presets / sizeof *theme_presets; i++)
        if (!strcmp(theme_presets[i].name, want)) { theme = theme_presets[i]; break; }
    int known = !strcmp(theme.name, want);
    if (cfg_dir[0]) {
        snprintf(path, sizeof path, "%s/themes", cfg_dir);
        if (themes_file_load(path, want, &theme)) known = 1;
    }
    if (!known && !cfg_warn[0])
        snprintf(cfg_warn, sizeof cfg_warn, "unknown theme \"%s\" — using tux", want);
    if (fresh && !cfg_warn[0])
        snprintf(cfg_warn, sizeof cfg_warn, "wrote default config to %s", cfg_dir);
}

/* ── git status ───────────────────────────────────────────────────── */
/* One `git status --porcelain` per refresh, cached in a flat sorted table
 * that the tree draw looks up by path. Directories inherit the "contains
 * something interesting" marker from their children. */
typedef struct { char *path; char st; } GitEnt;

static GitEnt *gitent = NULL;
static int     ngit = 0, gitcap = 0;
static int     git_repo = 0;
static char    git_branch[128] = "";

static int gitent_cmp(const void *a, const void *b) {
    return strcmp(((const GitEnt *)a)->path, ((const GitEnt *)b)->path);
}
/* Wrap a path for the shell. A directory name may legitimately contain a
 * quote, so close/escape/reopen rather than trusting the input. Returns 0 if
 * the result would not fit, in which case the caller skips the command. */
static int shq(char *out, size_t cap, const char *s) {
    size_t n = 0;
    if (cap < 3) return 0;
    out[n++] = '\'';
    for (; *s; s++) {
        if (*s == '\'') {
            if (n + 4 >= cap) return 0;
            memcpy(out + n, "'\\''", 4);
            n += 4;
        } else {
            if (n + 1 >= cap) return 0;
            out[n++] = *s;
        }
    }
    if (n + 2 > cap) return 0;
    out[n++] = '\'';
    out[n] = 0;
    return 1;
}
static void git_clear(void) {
    for (int i = 0; i < ngit; i++) free(gitent[i].path);
    ngit = 0;
    git_repo = 0;
    git_branch[0] = 0;
}
static void git_add(const char *rel, char st) {
    if (ngit == gitcap) {
        gitcap = gitcap ? gitcap * 2 : 128;
        gitent = xrealloc(gitent, (size_t)gitcap * sizeof *gitent);
    }
    gitent[ngit].path = xstrdup(rel);
    gitent[ngit].st = st;
    ngit++;
}
/* Reduce a two-column porcelain code to the single letter shown in the tree. */
static char git_code(const char *xy) {
    char x = xy[0], y = xy[1];
    if (x == '?' || y == '?') return '?';
    if (x == 'A' || y == 'A') return 'A';
    if (x == 'D' || y == 'D') return 'D';
    if (x == 'R' || y == 'R') return 'R';
    if (x != ' ' && x != '?') return 'S';      /* staged */
    return 'M';
}
static void git_refresh(void) {
    git_clear();
    if (!root) return;

    char qroot[PATH_MAX * 2 + 8], cmd[PATH_MAX * 2 + 96];
    if (!shq(qroot, sizeof qroot, root->path)) return;
    snprintf(cmd, sizeof cmd,
             "git -C %s rev-parse --abbrev-ref HEAD 2>/dev/null", qroot);
    FILE *f = popen(cmd, "r");
    if (f) {
        if (fgets(git_branch, sizeof git_branch, f)) {
            git_branch[strcspn(git_branch, "\r\n")] = 0;
            if (git_branch[0]) git_repo = 1;
        }
        pclose(f);
    }
    if (!git_repo) return;

    snprintf(cmd, sizeof cmd,
             "git -C %s status --porcelain 2>/dev/null", qroot);
    f = popen(cmd, "r");
    if (!f) return;
    char line[PATH_MAX + 8];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (strlen(line) < 4) continue;
        char st = git_code(line);
        char *p = line + 3;
        char *arrow = strstr(p, " -> ");      /* renames: take the new name */
        if (arrow) p = arrow + 4;
        size_t n = strlen(p);
        if (n && p[n - 1] == '/') p[n - 1] = 0;   /* untracked dir */
        if (*p == '"') {                          /* quoted path */
            p++;
            char *e = strrchr(p, '"');
            if (e) *e = 0;
        }
        git_add(p, st);
    }
    pclose(f);
    qsort(gitent, (size_t)ngit, sizeof *gitent, gitent_cmp);
}
/* Status letter for an absolute path, or 0. Directories report the status of
 * whatever lies beneath them so a change is visible while collapsed. */
static char git_status_for(const char *abs, int is_dir) {
    if (!git_repo || !ngit || !root) return 0;
    size_t rl = strlen(root->path);
    if (strncmp(abs, root->path, rl) != 0) return 0;
    const char *rel = abs + rl;
    while (*rel == '/') rel++;
    if (!*rel) return 0;

    if (!is_dir) {
        GitEnt key;
        key.path = (char *)rel;
        GitEnt *hit = bsearch(&key, gitent, (size_t)ngit, sizeof *gitent, gitent_cmp);
        return hit ? hit->st : 0;
    }
    /* directory: any entry under "rel/" counts */
    size_t n = strlen(rel);
    for (int i = 0; i < ngit; i++)
        if (strncmp(gitent[i].path, rel, n) == 0 && gitent[i].path[n] == '/')
            return gitent[i].st == '?' ? '?' : 'M';
    return 0;
}

/* ── terminal emulator ────────────────────────────────────────────── */
/* A pty-backed shell living in a tab. The parser covers the subset of VT100 /
 * xterm that interactive shells and full-screen programs actually rely on:
 * cursor motion, erase, scroll regions, insert/delete, SGR colors, the
 * alternate screen and OSC titles. Explicitly not handled: mouse reporting,
 * sixel/graphics, double-width line attributes. */
#define TERM_SB_MAX 1000       /* scrollback lines kept per terminal */

typedef struct { char b[4]; unsigned char n, at; short fg, bg; } Cell;

enum { TA_BOLD = 1, TA_UNDER = 2, TA_REV = 4, TA_DIM = 8 };
enum { PS_GROUND, PS_ESC, PS_CSI, PS_OSC, PS_CHARSET };

struct Term {
    int    fd;
    pid_t  pid;
    int    rows, cols;
    Cell  *g;                  /* rows*cols, the visible screen */
    Cell  *alt;                /* alternate screen, allocated on first use */
    int    alt_on;
    int    cy, cx;
    int    scy, scx;           /* saved cursor (ESC 7 / CSI s) */
    unsigned char at; short fg, bg;
    int    top, bot;           /* scroll region, inclusive */
    int    wrapnext;           /* deferred wrap: cursor sits past the last col */
    int    hidecur;
    Cell  *sb;                 /* scrollback ring, TERM_SB_MAX rows of `cols` */
    int    sb_n, sb_head, sb_view;
    int    ps, np, params[8], priv, uexp;
    char   osc[128]; int oscn;
    int    dead, status;
    char   title[64];
    char   pend[4]; int pendn; /* partial UTF-8 arriving from the pty */
};

static Cell term_blank(Term *t) {
    Cell c;
    c.b[0] = ' '; c.b[1] = c.b[2] = c.b[3] = 0;
    c.n = 1; c.at = 0; c.fg = -1; c.bg = t ? t->bg : -1;
    return c;
}
static Cell *term_row(Term *t, int y) { return t->g + (size_t)y * t->cols; }
static void term_clear_row(Term *t, int y, int from, int to) {
    if (y < 0 || y >= t->rows) return;
    Cell *r = term_row(t, y);
    Cell b = term_blank(t);
    for (int x = max2(0, from); x <= to && x < t->cols; x++) r[x] = b;
}
/* the line scrolling off the top of the region is kept for scrollback */
static void term_to_scrollback(Term *t, int y) {
    if (!t->sb || t->alt_on) return;
    memcpy(t->sb + (size_t)t->sb_head * t->cols, term_row(t, y),
           (size_t)t->cols * sizeof(Cell));
    t->sb_head = (t->sb_head + 1) % TERM_SB_MAX;
    if (t->sb_n < TERM_SB_MAX) t->sb_n++;
}
static void term_scroll_up(Term *t, int n) {
    for (int k = 0; k < n; k++) {
        term_to_scrollback(t, t->top);
        for (int y = t->top; y < t->bot; y++)
            memcpy(term_row(t, y), term_row(t, y + 1), (size_t)t->cols * sizeof(Cell));
        term_clear_row(t, t->bot, 0, t->cols - 1);
    }
}
static void term_scroll_down(Term *t, int n) {
    for (int k = 0; k < n; k++) {
        for (int y = t->bot; y > t->top; y--)
            memcpy(term_row(t, y), term_row(t, y - 1), (size_t)t->cols * sizeof(Cell));
        term_clear_row(t, t->top, 0, t->cols - 1);
    }
}
static void term_newline(Term *t) {
    if (t->cy == t->bot) term_scroll_up(t, 1);
    else if (t->cy < t->rows - 1) t->cy++;
}
static void term_put(Term *t, const char *b, int n) {
    if (t->wrapnext) { t->cx = 0; term_newline(t); t->wrapnext = 0; }
    if (t->cx >= t->cols) t->cx = t->cols - 1;
    Cell *c = &term_row(t, t->cy)[t->cx];
    int k = n > 4 ? 4 : n;
    memcpy(c->b, b, (size_t)k);
    c->n = (unsigned char)k;
    c->at = t->at; c->fg = t->fg; c->bg = t->bg;
    if (t->cx + 1 >= t->cols) t->wrapnext = 1;      /* defer the wrap, like xterm */
    else t->cx++;
}
static void term_sgr(Term *t) {
    if (!t->np) { t->np = 1; t->params[0] = 0; }
    for (int i = 0; i < t->np; i++) {
        int p = t->params[i];
        if (p == 0)      { t->at = 0; t->fg = -1; t->bg = -1; }
        else if (p == 1) t->at |= TA_BOLD;
        else if (p == 2) t->at |= TA_DIM;
        else if (p == 4) t->at |= TA_UNDER;
        else if (p == 7) t->at |= TA_REV;
        else if (p == 22) t->at &= (unsigned char)~(TA_BOLD | TA_DIM);
        else if (p == 24) t->at &= (unsigned char)~TA_UNDER;
        else if (p == 27) t->at &= (unsigned char)~TA_REV;
        else if (p >= 30 && p <= 37)   t->fg = (short)(p - 30);
        else if (p >= 40 && p <= 47)   t->bg = (short)(p - 40);
        else if (p >= 90 && p <= 97)   t->fg = (short)(p - 90 + 8);
        else if (p >= 100 && p <= 107) t->bg = (short)(p - 100 + 8);
        else if (p == 39) t->fg = -1;
        else if (p == 49) t->bg = -1;
        else if ((p == 38 || p == 48) && i + 1 < t->np) {
            short v = -1;
            if (t->params[i + 1] == 5 && i + 2 < t->np) {
                v = (short)t->params[i + 2]; i += 2;
            } else if (t->params[i + 1] == 2 && i + 4 < t->np) {
                int rgb = (t->params[i+2] << 16) | (t->params[i+3] << 8) | t->params[i+4];
                v = (short)rgb_to_256(rgb);        /* fold truecolor to 256 */
                i += 4;
            }
            if (v >= 0) { if (p == 38) t->fg = v; else t->bg = v; }
        }
    }
}
static void term_use_alt(Term *t, int on) {
    if (on == t->alt_on) return;
    if (!t->alt) {
        t->alt = xmalloc((size_t)t->rows * t->cols * sizeof(Cell));
        Cell b = term_blank(t);
        for (int i = 0; i < t->rows * t->cols; i++) t->alt[i] = b;
    }
    Cell *tmp = t->g; t->g = t->alt; t->alt = tmp;
    t->alt_on = on;
    if (on) {
        Cell b = term_blank(t);
        for (int i = 0; i < t->rows * t->cols; i++) t->g[i] = b;
        t->cy = t->cx = 0;
    }
}
static void term_csi(Term *t, char f) {
    int p0 = t->np > 0 ? t->params[0] : 0;
    int p1 = t->np > 1 ? t->params[1] : 0;
    int n  = p0 ? p0 : 1;
    switch (f) {
        case 'A': t->cy = max2(t->top, t->cy - n); t->wrapnext = 0; break;
        case 'B': t->cy = min2(t->bot, t->cy + n); t->wrapnext = 0; break;
        case 'C': t->cx = min2(t->cols - 1, t->cx + n); t->wrapnext = 0; break;
        case 'D': t->cx = max2(0, t->cx - n); t->wrapnext = 0; break;
        case 'E': t->cy = min2(t->bot, t->cy + n); t->cx = 0; break;
        case 'F': t->cy = max2(t->top, t->cy - n); t->cx = 0; break;
        case 'G': t->cx = min2(t->cols - 1, max2(0, n - 1)); t->wrapnext = 0; break;
        case 'd': t->cy = min2(t->rows - 1, max2(0, n - 1)); t->wrapnext = 0; break;
        case 'H': case 'f':
            t->cy = min2(t->rows - 1, max2(0, (p0 ? p0 : 1) - 1));
            t->cx = min2(t->cols - 1, max2(0, (p1 ? p1 : 1) - 1));
            t->wrapnext = 0;
            break;
        case 'J':
            if (p0 == 0) {
                term_clear_row(t, t->cy, t->cx, t->cols - 1);
                for (int y = t->cy + 1; y < t->rows; y++) term_clear_row(t, y, 0, t->cols - 1);
            } else if (p0 == 1) {
                term_clear_row(t, t->cy, 0, t->cx);
                for (int y = 0; y < t->cy; y++) term_clear_row(t, y, 0, t->cols - 1);
            } else {
                for (int y = 0; y < t->rows; y++) term_clear_row(t, y, 0, t->cols - 1);
            }
            break;
        case 'K':
            if (p0 == 0)      term_clear_row(t, t->cy, t->cx, t->cols - 1);
            else if (p0 == 1) term_clear_row(t, t->cy, 0, t->cx);
            else              term_clear_row(t, t->cy, 0, t->cols - 1);
            break;
        case 'L': { int s = t->top; t->top = t->cy;      /* insert lines */
                    term_scroll_down(t, min2(n, t->bot - t->cy + 1)); t->top = s; break; }
        case 'M': { int s = t->top; t->top = t->cy;      /* delete lines */
                    term_scroll_up(t, min2(n, t->bot - t->cy + 1)); t->top = s; break; }
        case 'P': {                                     /* delete chars */
            Cell *r = term_row(t, t->cy);
            int k = min2(n, t->cols - t->cx);
            memmove(r + t->cx, r + t->cx + k,
                    (size_t)(t->cols - t->cx - k) * sizeof(Cell));
            term_clear_row(t, t->cy, t->cols - k, t->cols - 1);
            break;
        }
        case '@': {                                     /* insert blanks */
            Cell *r = term_row(t, t->cy);
            int k = min2(n, t->cols - t->cx);
            memmove(r + t->cx + k, r + t->cx,
                    (size_t)(t->cols - t->cx - k) * sizeof(Cell));
            term_clear_row(t, t->cy, t->cx, t->cx + k - 1);
            break;
        }
        case 'X': term_clear_row(t, t->cy, t->cx, min2(t->cols - 1, t->cx + n - 1)); break;
        case 'S': term_scroll_up(t, n);   break;
        case 'T': term_scroll_down(t, n); break;
        case 'r':
            t->top = max2(0, (p0 ? p0 : 1) - 1);
            t->bot = min2(t->rows - 1, (p1 ? p1 : t->rows) - 1);
            if (t->top >= t->bot) { t->top = 0; t->bot = t->rows - 1; }
            t->cy = t->top; t->cx = 0;
            break;
        case 'm': term_sgr(t); break;
        case 's': t->scy = t->cy; t->scx = t->cx; break;
        case 'u': t->cy = t->scy; t->cx = t->scx; break;
        case 'h': case 'l':
            if (t->priv) {
                int on = (f == 'h');
                if (p0 == 25) t->hidecur = !on;
                else if (p0 == 1049 || p0 == 47 || p0 == 1047) term_use_alt(t, on);
            }
            break;
        default: break;
    }
}
/* run bytes from the pty through the parser */
static void term_feed(Term *t, const char *s, int n) {
    for (int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (t->ps) {
            case PS_GROUND:
                if (c == 0x1b) { t->ps = PS_ESC; t->np = 0; t->priv = 0; t->oscn = 0; }
                else if (c == '\n' || c == 0x0b || c == 0x0c) { t->wrapnext = 0; term_newline(t); }
                else if (c == '\r') { t->cx = 0; t->wrapnext = 0; }
                else if (c == '\b') { if (t->cx > 0) t->cx--; t->wrapnext = 0; }
                else if (c == '\t') {
                    t->cx = min2(t->cols - 1, (t->cx / 8 + 1) * 8);
                    t->wrapnext = 0;
                }
                else if (c < 32 || c == 127) { /* other C0: ignore */ }
                else if (c < 0x80) { char b = (char)c; term_put(t, &b, 1); }
                else {
                    /* gather a UTF-8 sequence so it lands in one cell */
                    if (t->pendn == 0) {
                        t->uexp = (c >= 0xf0) ? 4 : (c >= 0xe0) ? 3 : 2;
                        t->pend[0] = (char)c; t->pendn = 1;
                    } else if (t->pendn < 4) {
                        t->pend[t->pendn++] = (char)c;
                    }
                    if (t->pendn >= t->uexp) { term_put(t, t->pend, t->pendn); t->pendn = 0; }
                }
                break;
            case PS_ESC:
                if (c == '[') { t->ps = PS_CSI; t->np = 0; t->params[0] = 0; t->priv = 0; }
                else if (c == ']') { t->ps = PS_OSC; t->oscn = 0; }
                else if (c == '(' || c == ')' || c == '*' || c == '+') t->ps = PS_CHARSET;
                else {
                    if (c == 'M') {                     /* reverse index */
                        if (t->cy == t->top) term_scroll_down(t, 1);
                        else if (t->cy > 0) t->cy--;
                    } else if (c == '7') { t->scy = t->cy; t->scx = t->cx; }
                    else if (c == '8') { t->cy = t->scy; t->cx = t->scx; }
                    else if (c == 'c') {                /* reset */
                        t->at = 0; t->fg = t->bg = -1;
                        t->top = 0; t->bot = t->rows - 1;
                        for (int y = 0; y < t->rows; y++) term_clear_row(t, y, 0, t->cols - 1);
                        t->cy = t->cx = 0;
                    }
                    t->ps = PS_GROUND;
                }
                break;
            case PS_CHARSET: t->ps = PS_GROUND; break;
            case PS_CSI:
                if (c == '?' || c == '>' || c == '!' || c == '$' || c == '"' || c == '\'')
                    t->priv = 1;
                else if (isdigit(c)) {
                    if (t->np == 0) t->np = 1;
                    if (t->np <= 8) t->params[t->np - 1] = t->params[t->np - 1] * 10 + (c - '0');
                } else if (c == ';' || c == ':') {
                    if (t->np == 0) t->np = 1;
                    if (t->np < 8) t->params[t->np++] = 0;
                } else if (c >= 0x40 && c <= 0x7e) {
                    term_csi(t, (char)c);
                    t->ps = PS_GROUND;
                }
                break;
            case PS_OSC:
                if (c == 7 || c == 0x1b) {              /* BEL or start of ST */
                    t->osc[t->oscn] = 0;
                    if ((t->osc[0] == '0' || t->osc[0] == '2') && t->osc[1] == ';')
                        snprintf(t->title, sizeof t->title, "%s", t->osc + 2);
                    t->ps = PS_GROUND;
                } else if (t->oscn + 1 < (int)sizeof t->osc) {
                    t->osc[t->oscn++] = (char)c;
                }
                break;
        }
    }
}

/* ── terminal lifecycle ───────────────────────────────────────────── */
static void term_size(Term *t, int rows, int cols) {
    if (rows < 1) rows = 1;
    if (cols < 1) cols = 1;
    if (t->g && rows == t->rows && cols == t->cols) return;

    Cell *ng = xmalloc((size_t)rows * cols * sizeof(Cell));
    Cell blank = term_blank(t);
    for (int i = 0; i < rows * cols; i++) ng[i] = blank;
    if (t->g) {                             /* keep what still fits */
        int keep = min2(rows, t->rows);
        int srcy = t->rows - keep, dsty = rows - keep;
        for (int y = 0; y < keep; y++)
            memcpy(ng + (size_t)(dsty + y) * cols,
                   t->g + (size_t)(srcy + y) * t->cols,
                   (size_t)min2(cols, t->cols) * sizeof(Cell));
        free(t->g);
        t->cy = max2(0, min2(rows - 1, t->cy - srcy));
    }
    t->g = ng;
    free(t->alt); t->alt = NULL;            /* rebuilt on next use */
    t->alt_on = 0;

    /* the scrollback ring is row-width-sensitive; simplest correct thing on a
     * width change is to start it over rather than reflow */
    if (!t->sb || cols != t->cols) {
        free(t->sb);
        t->sb = xmalloc((size_t)TERM_SB_MAX * cols * sizeof(Cell));
        for (int i = 0; i < TERM_SB_MAX * cols; i++) t->sb[i] = blank;
        t->sb_n = t->sb_head = t->sb_view = 0;
    }
    t->rows = rows; t->cols = cols;
    t->top = 0; t->bot = rows - 1;
    t->cx = min2(t->cx, cols - 1);
    t->cy = min2(t->cy, rows - 1);
    if (t->fd >= 0) {
        struct winsize ws = { (unsigned short)rows, (unsigned short)cols, 0, 0 };
        ioctl(t->fd, TIOCSWINSZ, &ws);
    }
}
static Term *term_new(int rows, int cols, const char *cwd) {
    Term *t = calloc(1, sizeof *t);
    if (!t) die("out of memory");
    t->fd = -1; t->fg = t->bg = -1;
    t->rows = t->cols = 0;
    snprintf(t->title, sizeof t->title, "shell");
    term_size(t, rows, cols);

    struct winsize ws = { (unsigned short)rows, (unsigned short)cols, 0, 0 };
    pid_t pid = forkpty(&t->fd, NULL, NULL, &ws);
    if (pid < 0) { free(t->g); free(t->sb); free(t); return NULL; }
    if (pid == 0) {                          /* child: become the shell */
        if (cwd) { if (chdir(cwd) != 0) { /* stay put */ } }
        setenv("TERM", "xterm-256color", 1);
        unsetenv("LINES"); unsetenv("COLUMNS");
        const char *sh = getenv("SHELL");
        if (!sh || !*sh) sh = "/bin/sh";
        execl(sh, sh, "-i", (char *)NULL);
        execl("/bin/sh", "sh", (char *)NULL);
        _exit(127);
    }
    t->pid = pid;
    fcntl(t->fd, F_SETFL, O_NONBLOCK);
    return t;
}
static void term_free(Term *t) {
    if (!t) return;
    if (t->fd >= 0) close(t->fd);
    if (t->pid > 0) {
        kill(t->pid, SIGHUP);
        waitpid(t->pid, NULL, WNOHANG);
    }
    free(t->g); free(t->alt); free(t->sb);
    free(t);
}
/* Drain whatever the shell has produced. Returns 1 if the screen changed. */
static int term_pump(Term *t) {
    if (!t || t->fd < 0 || t->dead) return 0;
    char buf[8192];
    int changed = 0;
    for (int guard = 0; guard < 64; guard++) {   /* bounded: stay responsive */
        ssize_t n = read(t->fd, buf, sizeof buf);
        if (n > 0) { term_feed(t, buf, (int)n); changed = 1; continue; }
        if (n == 0) { t->dead = 1; changed = 1; break; }
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        if (errno == EINTR) continue;
        t->dead = 1; changed = 1; break;         /* EIO: child exited */
    }
    if (t->dead && t->pid > 0) {
        int st = 0;
        if (waitpid(t->pid, &st, WNOHANG) == t->pid) { t->status = st; t->pid = 0; }
    }
    return changed;
}
static void term_write(Term *t, const char *s, int n) {
    if (!t || t->fd < 0 || t->dead) return;
    while (n > 0) {
        ssize_t w = write(t->fd, s, (size_t)n);
        if (w <= 0) { if (errno == EINTR) continue; break; }
        s += w; n -= (int)w;
    }
}
/* Translate an sds key code into the bytes a real terminal would send. */
static void term_key(Term *t, int c) {
    char b[16];
    t->sb_view = 0;                       /* any keypress jumps back to live */
    switch (c) {
        case KEY_UP:    term_write(t, "\033[A", 3); return;
        case KEY_DOWN:  term_write(t, "\033[B", 3); return;
        case KEY_RIGHT: term_write(t, "\033[C", 3); return;
        case KEY_LEFT:  term_write(t, "\033[D", 3); return;
        case KEY_HOME:  term_write(t, "\033[H", 3); return;
        case KEY_END:   term_write(t, "\033[F", 3); return;
        case KEY_PPAGE: term_write(t, "\033[5~", 4); return;
        case KEY_NPAGE: term_write(t, "\033[6~", 4); return;
        case KEY_DC:    term_write(t, "\033[3~", 4); return;
        case KEY_IC:    term_write(t, "\033[2~", 4); return;
        case KEY_BTAB:  term_write(t, "\033[Z", 3); return;
        case KEY_BACKSPACE: case 127: case 8: term_write(t, "\177", 1); return;
        case '\r': case '\n': case KEY_ENTER: term_write(t, "\r", 1); return;
        default: break;
    }
    if (c >= KEY_F(1) && c <= KEY_F(12)) {
        static const char *fk[12] = {
            "\033OP", "\033OQ", "\033OR", "\033OS", "\033[15~", "\033[17~",
            "\033[18~", "\033[19~", "\033[20~", "\033[21~", "\033[23~", "\033[24~"
        };
        const char *s = fk[c - KEY_F(1)];
        term_write(t, s, (int)strlen(s));
        return;
    }
    if (c >= ALT(0) && c <= ALT(255)) {          /* Alt+x → ESC x */
        b[0] = 27; b[1] = (char)(c - ALT(0));
        term_write(t, b, 2);
        return;
    }
    if (c >= 0 && c < 256) { b[0] = (char)c; term_write(t, b, 1); }
}
/* Open a shell in a new tab, sized to the current editor pane. */
static void open_terminal(void) {
    if (ntabs == MAX_TABS) { set_msg("too many open tabs", NULL); return; }
    int x0 = tree_hidden ? 0 : tree_w + 1;
    int rows = max2(1, LINES - 2), cols = max2(1, COLS - x0);
    Term *t = term_new(rows, cols, root ? root->path : NULL);
    if (!t) { set_msg("could not start a shell", NULL); return; }

    Buf *b = calloc(1, sizeof *b);
    if (!b) die("out of memory");
    b->kind = TAB_TERM;
    b->term = t;
    b->lang = LANG_TEXT;
    /* one empty line so any stray editor-side code still sees a sane buffer */
    b->ln = xmalloc(sizeof(Line));
    b->ln[0].s = xmalloc(1); b->ln[0].s[0] = 0;
    b->ln[0].len = 0; b->ln[0].cap = 1; b->ln[0].hst = 0;
    b->n = b->cap = 1;
    snprintf(b->name, sizeof b->name, "shell");
    snprintf(b->path, sizeof b->path, "%s", root ? root->path : "");
    tabs[ntabs++] = b;
    set_cur(ntabs - 1);
    set_msg("terminal opened — type 'exit' to close", NULL);
}
/* Any live terminal means the main loop must poll rather than block. */
static int any_live_term(void) {
    for (int i = 0; i < ntabs; i++)
        if (tabs[i]->kind == TAB_TERM && tabs[i]->term && !tabs[i]->term->dead)
            return 1;
    return 0;
}
static int pump_all_terms(void) {
    int changed = 0;
    for (int i = 0; i < ntabs; i++)
        if (tabs[i]->kind == TAB_TERM) changed |= term_pump(tabs[i]->term);
    return changed;
}

/* ── rendering ────────────────────────────────────────────────────── */

static int rx_of(Line *l, int cx) {
    int rx = 0;
    for (int i = 0; i < cx && i < l->len; i++)
        rx = (l->s[i] == '\t') ? rx + tabstop - rx % tabstop : rx + 1;
    return rx;
}
static int cx_of_rx(Line *l, int rx) {       /* render col → byte index */
    int cur = 0;
    for (int i = 0; i < l->len; i++) {
        cur = (l->s[i] == '\t') ? cur + tabstop - cur % tabstop : cur + 1;
        if (cur > rx) return i;
    }
    return l->len;
}
/* screen rows a buffer line occupies (1 when not wrapping) */
static int line_rows(Buf *b, int li, int tw) {
    if (!wrap || tw < 1) return 1;
    int n = rx_of(&b->ln[li], b->ln[li].len);
    return n < 1 ? 1 : (n + tw - 1) / tw;
}
static attr_t attr_for(int ha, int ov) {
    attr_t a;
    switch (ha) {
        case HA_KW:   a = COLOR_PAIR(CP_KW) | A_BOLD; break;
        case HA_TYPE: a = COLOR_PAIR(CP_TYPE);        break;
        case HA_STR:  a = COLOR_PAIR(CP_STR);         break;
        case HA_COM:  a = COLOR_PAIR(CP_COM);         break;
        case HA_NUM:  a = COLOR_PAIR(CP_NUM);         break;
        case HA_PRE:  a = COLOR_PAIR(CP_PRE);         break;
        default:      a = A_NORMAL;
    }
    if (ov & OV_FIND) a = COLOR_PAIR(CP_FIND);
    if (ov & OV_SEL)  a |= A_REVERSE;
    if (ov & OV_BRK)  a |= A_BOLD | A_UNDERLINE;
    return a;
}
/* case-insensitive memmem */
static int ci_find(const char *hay, int hlen, const char *nee, int nlen, int from) {
    if (nlen == 0 || nlen > hlen) return -1;
    for (int i = from; i + nlen <= hlen; i++) {
        int k = 0;
        while (k < nlen &&
               tolower((unsigned char)hay[i+k]) == tolower((unsigned char)nee[k]))
            k++;
        if (k == nlen) return i;
    }
    return -1;
}
static int draw_row(Buf *b, int scr_y, int scr_x, int li, int tw, int maxrows,
                    int startseg) {
    static unsigned char *hat = NULL, *ov = NULL;
    static char *ech = NULL; static unsigned char *eat = NULL, *eov = NULL;
    static int cap = 0;
    Line *l = &b->ln[li];
    int need = l->len + 8;
    if (need > cap) {
        cap = need * 2;
        hat = xrealloc(hat, (size_t)cap);
        ov  = xrealloc(ov,  (size_t)cap);
        ech = xrealloc(ech, (size_t)cap * TABSTOP_MAX + 8);
        eat = xrealloc(eat, (size_t)cap * TABSTOP_MAX + 8);
        eov = xrealloc(eov, (size_t)cap * TABSTOP_MAX + 8);
    }
    hl_line(b, li, hat);
    memset(ov, 0, (size_t)(l->len ? l->len : 1));

    int y1, x1, y2, x2;                                  /* selection */
    if (sel_norm(b, &y1, &x1, &y2, &x2) && li >= y1 && li <= y2) {
        int a = (li == y1) ? x1 : 0;
        int z = (li == y2) ? x2 : l->len;
        for (int i = a; i < z && i < l->len; i++) ov[i] |= OV_SEL;
    }
    if (find_show && findq[0]) {                          /* find matches */
        int q = (int)strlen(findq), at = 0;
        while ((at = ci_find(l->s, l->len, findq, q, at)) >= 0) {
            for (int i = at; i < at + q; i++) ov[i] |= OV_FIND;
            at += q;
        }
    }
    if (brk_y1 == li && brk_x1 < l->len) ov[brk_x1] |= OV_BRK;
    if (brk_y2 == li && brk_x2 < l->len) ov[brk_x2] |= OV_BRK;

    /* expand tabs, carrying attrs/overlays along */
    int n = 0;
    for (int i = 0; i < l->len; i++) {
        if (l->s[i] == '\t') {
            do { ech[n] = ' '; eat[n] = hat[i]; eov[n] = ov[i]; n++; }
            while (n % tabstop);
        } else { ech[n] = l->s[i]; eat[n] = hat[i]; eov[n] = ov[i]; n++; }
    }
    if (!wrap) {
        if (n > b->coloff) {
            int from = b->coloff, to = min2(n, b->coloff + tw);
            for (int i = from; i < to; ) {
                int j = i;
                while (j < to && eat[j] == eat[i] && eov[j] == eov[i]) j++;
                attrset(attr_for(eat[i], eov[i]));
                mvaddnstr(scr_y, scr_x + (i - from), ech + i, j - i);
                i = j;
            }
            attrset(A_NORMAL);
        }
        return 1;
    }
    int used = 0;
    for (int from = startseg * tw; used < maxrows; from += tw) {
        int to = min2(n, from + tw);
        for (int i = from; i < to; ) {
            int j = i;
            while (j < to && eat[j] == eat[i] && eov[j] == eov[i]) j++;
            attrset(attr_for(eat[i], eov[i]));
            mvaddnstr(scr_y + used, scr_x + (i - from), ech + i, j - i);
            i = j;
        }
        used++;
        if (to >= n) break;
    }
    attrset(A_NORMAL);
    return used ? used : 1;
}

static void draw_tabbar(int w) {
    move(0, 0);
    attron(COLOR_PAIR(CP_TAB));
    for (int i = 0; i < w; i++) addch(' ');
    attroff(COLOR_PAIR(CP_TAB));
    int first = 0;
    for (;;) {
        int x = 0, fits = 0;
        for (int i = first; i < ntabs; i++) {
            int tw = (int)strlen(tabs[i]->name) + 4;
            if (i == cur && x + tw <= w) fits = 1;
            x += tw;
        }
        if (fits || first >= cur || first >= ntabs - 1) break;
        first++;
    }
    int x = 0;
    for (int i = first; i < ntabs && x < w; i++) {
        char t[NAME_MAX + 8];
        if (tabs[i]->kind == TAB_TERM) {
            Term *tm = tabs[i]->term;
            snprintf(t, sizeof t, " >_ %s%s ",
                     tm && tm->title[0] ? tm->title : "shell",
                     tm && tm->dead ? " (exited)" : "");
        } else {
            snprintf(t, sizeof t, " %s%s ", tabs[i]->name,
                     tabs[i]->dirty ? "*" : "");
        }
        /* a tab open in some other pane is underlined, so the split is
         * readable from the tab bar alone */
        int shown = 0;
        for (int p = 0; p < npanes; p++) if (panes[p] == i) shown = 1;
        int pair = (i == cur) ? CP_TAB_ACT : CP_TAB;
        attr_t extra = (i == cur) ? A_BOLD
                     : (shown ? (A_UNDERLINE | A_BOLD) : A_NORMAL);
        attron(COLOR_PAIR(pair) | extra);
        mvaddnstr(0, x, t, w - x);
        attroff(COLOR_PAIR(pair) | extra);
        x += (int)strlen(t);
        if (x < w) {
            attron(COLOR_PAIR(CP_MUTED)); mvaddstr(0, x, "|");
            attroff(COLOR_PAIR(CP_MUTED)); x++;
        }
    }
    if (ntabs == 0) {
        attron(COLOR_PAIR(CP_TAB));
        mvaddnstr(0, 1, "sds — no file open", w - 1);
        attroff(COLOR_PAIR(CP_TAB));
    }
}
static void draw_tree(int h) {
    if (tree_hidden) return;
    int rows = h - 2;
    if (tsel < toff) toff = tsel;
    if (tsel >= toff + rows) toff = tsel - rows + 1;
    for (int r = 0; r < rows; r++) {
        int i = toff + r;
        move(1 + r, 0);
        clrtoeol();
        attron(COLOR_PAIR(CP_MUTED));
        mvaddch(1 + r, tree_w, ACS_VLINE);
        attroff(COLOR_PAIR(CP_MUTED));
        if (i >= nvis) continue;
        Node *n = vis[i];
        char line[512];
        const char *mark = n->is_dir ? (n->expanded ? "v " : "> ") : "  ";
        snprintf(line, sizeof line, "%*s%s%s", n->depth * 2, "", mark, n->name);
        if (i == tsel) attron(COLOR_PAIR(CP_SEL) | A_BOLD);
        else if (n->is_dir) attron(COLOR_PAIR(CP_DIR));
        mvaddnstr(1 + r, 1, line, tree_w - 2);
        if (i == tsel) {
            int len = (int)strlen(line);
            for (int x = 1 + len; x < tree_w - 1; x++) mvaddch(1 + r, x, ' ');
        }
        if (i == tsel) attroff(COLOR_PAIR(CP_SEL) | A_BOLD);
        else if (n->is_dir) attroff(COLOR_PAIR(CP_DIR));
        /* git marker in the last column, so it survives long names */
        char gs = git_status_for(n->path, n->is_dir);
        if (gs && tree_w >= 4) {
            int pair = (gs == '?') ? CP_MUTED : (gs == 'D') ? CP_ERR : CP_LINENO;
            if (i == tsel) pair = CP_SEL;
            attron(COLOR_PAIR(pair) | A_BOLD);
            mvaddch(1 + r, tree_w - 1, (chtype)gs);
            attroff(COLOR_PAIR(pair) | A_BOLD);
        }
    }
}
/* bracket matching for the highlight */
static void find_bracket(Buf *b) {
    brk_y1 = -1;
    const char *op = "([{", *cl = ")]}";
    int y = b->cy, x = -1;
    char c = 0;
    Line *l = &b->ln[y];
    if (b->cx < l->len && strchr("([{)]}", l->s[b->cx])) { x = b->cx; c = l->s[x]; }
    else if (b->cx > 0 && strchr("([{)]}", l->s[b->cx - 1])) { x = b->cx - 1; c = l->s[x]; }
    if (x < 0) return;
    const char *p;
    int fwd, depth = 0, steps = 0;
    char open, close;
    if ((p = strchr(op, c))) { fwd = 1; open = c; close = cl[p - op]; }
    else { p = strchr(cl, c); fwd = 0; close = c; open = op[p - cl]; }
    int sy = y, sx = x;
    while (steps++ < 200000) {
        if (fwd) { sx++; while (sy < b->n && sx >= b->ln[sy].len) { sy++; sx = 0; } if (sy >= b->n) return; }
        else     { sx--; while (sx < 0) { if (--sy < 0) return; sx = b->ln[sy].len - 1; } if (sx < 0) continue; }
        char d = b->ln[sy].s[sx];
        if (d == (fwd ? open : close)) depth++;
        else if (d == (fwd ? close : open)) {
            if (depth == 0) {
                brk_y1 = y; brk_x1 = x; brk_y2 = sy; brk_x2 = sx;
                return;
            }
            depth--;
        }
    }
}
/* ncurses wants a pair number, the terminal gives us (fg,bg) pairs on demand.
 * Allocate them lazily from a small cache above the app's fixed pairs. */
#define CP_TERM_BASE 20
static short term_pair(short fg, short bg) {
    static struct { short fg, bg; } cache[160];
    static int ncache = 0;
    if (fg < 0 && bg < 0) return 0;
    if (fg >= COLORS) fg = (short)(COLORS - 1);
    if (bg >= COLORS) bg = (short)(COLORS - 1);
    for (int i = 0; i < ncache; i++)
        if (cache[i].fg == fg && cache[i].bg == bg)
            return (short)(CP_TERM_BASE + i);
    if (ncache >= (int)(sizeof cache / sizeof *cache) ||
        CP_TERM_BASE + ncache >= COLOR_PAIRS) return 0;
    cache[ncache].fg = fg; cache[ncache].bg = bg;
    init_pair((short)(CP_TERM_BASE + ncache), fg, bg);
    ncache++;
    return (short)(CP_TERM_BASE + ncache - 1);
}
/* Draw one terminal tab. Rows above the live screen come from scrollback when
 * the user has scrolled back with Shift+PgUp. */
static void draw_term(Buf *b, int ytop, int x0, int rows, int cols) {
    Term *t = b->term;
    if (!t) return;
    if (t->rows != rows || t->cols != cols) term_size(t, rows, cols);
    if (t->sb_view > t->sb_n) t->sb_view = t->sb_n;

    for (int r = 0; r < rows; r++) {
        Cell *row;
        if (r < t->sb_view) {
            int li = t->sb_n - t->sb_view + r;             /* oldest = 0 */
            int pos = (t->sb_head - t->sb_n + li + 2 * TERM_SB_MAX) % TERM_SB_MAX;
            row = t->sb + (size_t)pos * t->cols;
        } else {
            int gy = r - t->sb_view;
            if (gy >= t->rows) break;
            row = term_row(t, gy);
        }
        move(ytop + r, x0);
        for (int x = 0; x < cols && x0 + x < COLS; x++) {
            Cell *c = &row[x];
            attr_t a = COLOR_PAIR(term_pair(c->fg, c->bg));
            if (c->at & TA_BOLD)  a |= A_BOLD;
            if (c->at & TA_DIM)   a |= A_DIM;
            if (c->at & TA_UNDER) a |= A_UNDERLINE;
            if (c->at & TA_REV)   a |= A_REVERSE;
            attrset(a);
            mvaddnstr(ytop + r, x0 + x, c->b, c->n ? c->n : 1);
        }
    }
    attrset(A_NORMAL);
    if (t->dead) {
        const char *m = " [process exited — Alt+W to close this tab] ";
        attron(COLOR_PAIR(CP_ERR) | A_BOLD);
        mvaddnstr(ytop + min2(t->cy + 1, rows - 1), x0, m, cols);
        attroff(COLOR_PAIR(CP_ERR) | A_BOLD);
    }
    if (!t->hidecur && !t->dead && t->sb_view == 0)
        move(ytop + min2(t->cy, rows - 1), x0 + min2(t->cx, cols - 1));
}

/* ── panes ────────────────────────────────────────────────────────── */
/* 1 pane fills the area; 2 split left/right; 3 keeps the left column whole
 * and stacks the right one; 4 is a 2×2. Dividers are carved out of the area
 * so the panes never overlap them. */
typedef struct { int y, x, h, w; } Rect;
typedef struct { Rect r[MAX_PANES]; int n, vx, hy, hx, hw; } Layout;

#define PANE_MINW 16
#define PANE_MINH 4

static Layout pane_layout(Rect a) {
    Layout L;
    memset(&L, 0, sizeof L);
    L.vx = L.hy = -1;
    L.n = npanes;
    if (L.n > MAX_PANES) L.n = MAX_PANES;
    if (L.n < 1) L.n = 1;

    int lw = (a.w - 1) / 2, rw = a.w - 1 - lw;
    int th = (a.h - 1) / 2, bh = a.h - 1 - th;
    /* too cramped to split usefully: show the focused pane alone */
    if (L.n >= 2 && (lw < PANE_MINW || rw < PANE_MINW)) L.n = 1;
    if (L.n >= 3 && (th < PANE_MINH || bh < PANE_MINH)) L.n = 2;
    if (L.n == 1) { L.r[0] = a; return L; }

    int xr = a.x + lw + 1;
    L.vx = a.x + lw;
    if (L.n == 2) {
        L.r[0] = (Rect){ a.y, a.x, a.h, lw };
        L.r[1] = (Rect){ a.y, xr,  a.h, rw };
        return L;
    }
    L.hy = a.y + th;
    if (L.n == 3) {
        L.r[0] = (Rect){ a.y,          a.x, a.h, lw };
        L.r[1] = (Rect){ a.y,          xr,  th,  rw };
        L.r[2] = (Rect){ a.y + th + 1, xr,  bh,  rw };
        L.hx = L.vx; L.hw = rw + 1;
    } else {
        L.r[0] = (Rect){ a.y,          a.x, th, lw };
        L.r[1] = (Rect){ a.y,          xr,  th, rw };
        L.r[2] = (Rect){ a.y + th + 1, a.x, bh, lw };
        L.r[3] = (Rect){ a.y + th + 1, xr,  bh, rw };
        L.hx = a.x; L.hw = a.w;
    }
    return L;
}
static Rect editor_area(void) {
    int x0 = tree_hidden ? 0 : tree_w + 1;
    Rect a = { 1, x0, LINES - 2, COLS - x0 };
    if (a.h < 1) a.h = 1;
    if (a.w < 1) a.w = 1;
    return a;
}
/* Nearest pane in a direction, by rectangle centre — distance along the axis
 * dominates, so a wide neighbour never wins over the one actually beside you. */
static void pane_focus_dir(int dir) {
    Layout L = pane_layout(editor_area());
    if (L.n < 2) { set_msg("only one pane open", NULL); return; }
    int from = min2(curpane, L.n - 1);
    double cx = L.r[from].x + L.r[from].w / 2.0;
    double cy = L.r[from].y + L.r[from].h / 2.0;
    int best = -1;
    double bd = 0;
    for (int i = 0; i < L.n; i++) {
        if (i == from) continue;
        double dx = (L.r[i].x + L.r[i].w / 2.0) - cx;
        double dy = (L.r[i].y + L.r[i].h / 2.0) - cy;
        int ok = (dir == D_LEFT  && dx < -0.5) || (dir == D_RIGHT && dx > 0.5) ||
                 (dir == D_UP    && dy < -0.5) || (dir == D_DOWN  && dy > 0.5);
        if (!ok) continue;
        int horiz = (dir == D_LEFT || dir == D_RIGHT);
        double along  = horiz ? dabs(dx) : dabs(dy);
        double across = horiz ? dabs(dy) : dabs(dx);
        double d = along + across * 4;
        if (best < 0 || d < bd) { best = i; bd = d; }
    }
    if (best >= 0) { curpane = best; cur = panes[curpane]; }
}

/* Where the focused pane wants the hardware cursor; -1 means "hide it". */
static int g_cy = -1, g_cx = -1;

static void draw_pane(Rect pr, int ti, int focused, int hdr) {
    if (pr.h < 1 || pr.w < 1 || ti < 0 || ti >= ntabs) return;
    Buf *b = tabs[ti];
    if (hdr) {
        char t[NAME_MAX + 16];
        if (b->kind == TAB_TERM)
            snprintf(t, sizeof t, " >_ %s ",
                     b->term && b->term->title[0] ? b->term->title : "shell");
        else
            snprintf(t, sizeof t, " %s%s ", b->name, b->dirty ? "*" : "");
        int pair = focused ? CP_TAB_ACT : CP_TAB;
        attron(COLOR_PAIR(pair));
        if (focused) attron(A_BOLD);
        move(pr.y, pr.x);
        for (int i = 0; i < pr.w; i++) addch(' ');
        mvaddnstr(pr.y, pr.x, t, pr.w);
        if (focused) attroff(A_BOLD);
        attroff(COLOR_PAIR(pair));
        pr.y++;
        pr.h--;
        if (pr.h < 1) return;
    }
    int rows = pr.h, x0 = pr.x, ew = pr.w, y0 = pr.y;
    if (b->kind == TAB_TERM) {
        draw_term(b, y0, x0, rows, ew);
        if (focused) getyx(stdscr, g_cy, g_cx);
        return;
    }
    /* line numbers make no sense for a PDF page, so it gets a plain margin */
    int nums = (b->kind != TAB_PDF);
    int gut = 1;
    if (nums) {
        for (int n = b->n; n; n /= 10) gut++;
        if (gut < 4) gut = 4;
        if (gut > 10) gut = 10;
    }
    int tw = ew - gut - 1;
    if (tw < 1) tw = 1;
    /* when wrapping, leave one column free so a cursor sitting at the wrap
     * point (rx == tw) still lands on screen instead of past the edge */
    if (wrap && tw > 1) tw--;
    g_wtw = tw;
    int rx = rx_of(&b->ln[b->cy], b->cx);

    if (!wrap) {
        if (b->cy < b->rowoff) b->rowoff = b->cy;
        if (b->cy >= b->rowoff + rows) b->rowoff = b->cy - rows + 1;
        if (rx < b->coloff) b->coloff = rx;
        if (rx >= b->coloff + tw) b->coloff = rx - tw + 1;
    } else {
        b->coloff = 0;
        int cseg = min2(rx / tw, line_rows(b, b->cy, tw) - 1);
        if (b->cy < b->rowoff) { b->rowoff = b->cy; b->subrow = 0; }
        /* cheap first guess so the loop below stays short on big jumps */
        if (b->cy - b->rowoff >= rows) { b->rowoff = max2(0, b->cy - rows + 1); b->subrow = 0; }
        if (b->rowoff == b->cy && b->subrow > cseg) b->subrow = cseg;
        if (b->subrow >= line_rows(b, b->rowoff, tw)) b->subrow = 0;
        for (;;) {                          /* scroll down a visual row at a time */
            int used = -b->subrow;
            for (int i = b->rowoff; i < b->cy; i++) used += line_rows(b, i, tw);
            used += cseg;
            if (used < rows) break;
            if (++b->subrow >= line_rows(b, b->rowoff, tw)) {
                b->subrow = 0;
                b->rowoff++;
            }
        }
    }

    find_bracket(b);
    ensure_hl(b, min2(b->rowoff + rows, b->n - 1));
#ifdef SDS_TREESITTER
    /* query only the window about to be drawn, not the whole file */
    ts_collect(b, b->rowoff, min2(b->rowoff + rows, b->n - 1));
#endif

    int vr = 0;
    for (int i = b->rowoff; i < b->n && vr < rows; i++) {
        int startseg = (wrap && i == b->rowoff) ? b->subrow : 0;
        if (nums && startseg == 0) {               /* number the line's first row */
            char num[16];
            snprintf(num, sizeof num, "%*d", gut, i + 1);
            attron(COLOR_PAIR(CP_LINENO));
            mvaddstr(y0 + vr, x0, num);
            attroff(COLOR_PAIR(CP_LINENO));
        }
        vr += draw_row(b, y0 + vr, x0 + gut + 1, i, tw, rows - vr, startseg);
    }
    if (!focused) return;
    if (wrap) {
        int cr = -b->subrow;
        for (int i = b->rowoff; i < b->cy; i++) cr += line_rows(b, i, tw);
        int seg = min2(rx / tw, line_rows(b, b->cy, tw) - 1);  /* clamp at EOL */
        cr += seg;
        cr = max2(0, min2(cr, rows - 1));
        g_cy = y0 + cr;
        g_cx = x0 + gut + 1 + (rx - seg * tw);
    } else {
        g_cy = y0 + (b->cy - b->rowoff);
        g_cx = x0 + gut + 1 + (rx - b->coloff);
    }
}
/* Draw every pane, focused one last so it owns g_wtw and the cursor. */
static void draw_editor(int h, int w) {
    (void)h; (void)w;
    Rect a = editor_area();
    g_cy = g_cx = -1;
    if (cur < 0) {
        const char *hint[] = {
            "Alt+Up/Down    browse the file tree",
            "Alt+Enter      open file / toggle folder",
            "Ctrl+P         quick-open by fuzzy name",
            "Alt+Shift+1..9 show a tab in its own pane",
            "Alt+H          all keybindings",
        };
        for (int i = 0; i < 5; i++) {
            attron(COLOR_PAIR(CP_MUTED));
            mvaddnstr(a.y + a.h / 2 - 2 + i, a.x + 3, hint[i], a.w - 3);
            attroff(COLOR_PAIR(CP_MUTED));
        }
        return;
    }
    Layout L = pane_layout(a);
    int focus = min2(curpane, L.n - 1);
    int hdr = L.n > 1;
    if (L.n == 1) {                       /* too cramped to split, or single */
        draw_pane(a, cur, 1, 0);
    } else {
        for (int i = 0; i < L.n; i++)
            if (i != focus) draw_pane(L.r[i], panes[i], 0, hdr);
        draw_pane(L.r[focus], panes[focus], 1, hdr);
    }
    attron(COLOR_PAIR(CP_MUTED));
    if (L.vx >= 0)
        for (int y = a.y; y < a.y + a.h; y++) mvaddch(y, L.vx, ACS_VLINE);
    if (L.hy >= 0)
        for (int x = L.hx; x < L.hx + L.hw && x < COLS; x++) {
            if (x == L.vx) mvaddch(L.hy, x, L.n == 3 ? ACS_LTEE : ACS_PLUS);
            else           mvaddch(L.hy, x, ACS_HLINE);
        }
    attroff(COLOR_PAIR(CP_MUTED));
}
static void draw_status(int h, int w) {
    attron(COLOR_PAIR(CP_STATUS));
    move(h - 1, 0);
    for (int i = 0; i < w; i++) addch(' ');
    char left[PATH_MAX + 192];
    if (cur >= 0 && tabs[cur]->kind == TAB_TERM) {
        Term *t = tabs[cur]->term;
        if (t && t->sb_view > 0)
            snprintf(left, sizeof left, " terminal   scrollback -%d/%d"
                     "   (any key returns to live)", t->sb_view, t->sb_n);
        else if (t && t->dead)
            snprintf(left, sizeof left, " terminal   exited (%d)",
                     WIFEXITED(t->status) ? WEXITSTATUS(t->status) : -1);
        else
            snprintf(left, sizeof left, " terminal   %s   Shift+PgUp scrollback",
                     t && t->title[0] ? t->title : "shell");
    } else if (cur >= 0 && tabs[cur]->kind == TAB_PDF) {
        Buf *b = tabs[cur];
        Pdf *p = b->pdf;
        snprintf(left, sizeof left, " %s   PDF   page %d/%d   Left/Right = page",
                 b->path, p->npg ? p->page + 1 : 0, p->npg);
    } else if (cur >= 0) {
        Buf *b = tabs[cur];
        char br[160] = "";
        if (git_repo) snprintf(br, sizeof br, "   %s", git_branch);
        snprintf(left, sizeof left, " %s%s   %s%s%s   %d:%d",
                 b->path, b->dirty ? " [+]" : "", b->lang->name,
                 wrap ? "  wrap" : "", br, b->cy + 1, b->cx + 1);
    } else {
        if (git_repo) snprintf(left, sizeof left, " %s   %s", root->path, git_branch);
        else          snprintf(left, sizeof left, " %s", root->path);
    }
    mvaddnstr(h - 1, 0, left, w);
    if (msg[0]) {
        attron(A_BOLD);
        mvaddnstr(h - 1, w / 2, msg, w / 2 - 1);
        attroff(A_BOLD);
    } else {
        const char *hint = "Alt+H help ";
        mvaddstr(h - 1, w - (int)strlen(hint), hint);
    }
    attroff(COLOR_PAIR(CP_STATUS));
}
static void draw_help(int h, int w) {
    static const char *lines[] = {
      "  sds — keybindings                                                ",
      "                                                                   ",
      "  FILE TREE                        EDITING                         ",
      "  Alt+Up/Down    move in tree      Ctrl+Z / Ctrl+Y   undo / redo   ",
      "  Alt+Rt/Left    expand/collapse   Ctrl+C/X/V        copy/cut/paste",
      "  Alt+Enter      open / toggle     Ctrl+A            select all    ",
      "  Alt+Insert     new file/folder   Ctrl+D            duplicate line",
      "  Alt+Delete     delete file/dir   Ctrl+K            delete line   ",
      "  F5 / Alt+E     rescan tree       Ctrl+/            toggle comment",
      "  Alt+B          show/hide sidebar Ctrl+Shift+Up/Dn  move line     ",
      "  (Alt+Left at top level hides it) Alt+O             new line below",
      "                                   Tab / Shift+Tab   indent/dedent ",
      "  TABS & PANES                     Shift+arrows      select        ",
      "  Alt+, / Alt+.  prev / next tab   Ctrl+Left/Right   word jump     ",
      "  Alt+1..9       go to tab N       Ctrl+Home/End     file start/end",
      "  Alt+W          close tab         Ctrl+Space        autocomplete  ",
      "  Alt+Shift+1..9 tab N in a pane   Alt+Z             toggle wrap   ",
      "  Alt+Shift+arrows  focus a pane                                   ",
      "  Alt+Shift+0    close this pane   PDF (read-only)                 ",
      "  (4 panes max, in a 2x2 grid;     Left/Right, n/p, Space  page    ",
      "   Alt+Shift+N again closes one)   Ctrl+G   go to page             ",
      "                                   Ctrl+F   search this page       ",
      "  FIND & GO                        Ctrl+C   copy selection         ",
      "  Ctrl+F  find (Enter=next)                                        ",
      "  F3      find next                TERMINAL                        ",
      "  Ctrl+R  replace (y/n/a/q)        Alt+T    new terminal tab       ",
      "  Ctrl+G  go to line               Shift+PgUp/PgDn   scrollback    ",
      "  Ctrl+P  quick-open file          type 'exit' to end the shell    ",
      "                                                                   ",
      "  Esc  clear selection/highlight   APP                             ",
      "  Tree marks: M modified  ? new    Ctrl+S / Alt+S    save          ",
      "              A added    D deleted Alt+R             run command   ",
      "                                   Alt+H             this help     ",
      "  config: ~/.config/sds/config     Alt+Q             quit          ",
    };
    int n = (int)(sizeof lines / sizeof *lines);
    int bw = (int)strlen(lines[0]) + 2, bh = n + 2;
    int y0 = (h - bh) / 2, x0 = (w - bw) / 2;
    if (y0 < 0) y0 = 0;
    if (x0 < 0) x0 = 0;
    attron(COLOR_PAIR(CP_SEL));
    for (int r = 0; r < bh && y0 + r < h; r++) {
        move(y0 + r, x0);
        for (int c = 0; c < bw && x0 + c < w; c++) addch(' ');
    }
    for (int i = 0; i < n && y0 + 1 + i < h; i++)
        mvaddnstr(y0 + 1 + i, x0 + 1, lines[i], w - x0 - 1);
    attroff(COLOR_PAIR(CP_SEL));
}
static void draw(void) {
    int h = LINES, w = COLS;
    erase();
    draw_tabbar(w);
    draw_tree(h);
    draw_status(h, w);
    draw_editor(h, w);
    if (show_help) draw_help(h, w);
    int wantcur = cur >= 0 && !show_help && g_cy >= 0 &&
                  tabs[cur]->kind != TAB_PDF;
    if (wantcur) move(g_cy, g_cx);
    curs_set(wantcur ? 1 : 0);
    refresh();
}

/* ── confirm dialog ───────────────────────────────────────────────── */
static int read_key(void);
/* Modal yes/no. `danger` paints the box red-ish and defaults to No.
 * y / n / Enter / arrows / Tab / Esc all behave as you'd expect.        */
/* Popups paint the app behind them exactly once and then only repaint their
 * own rectangle per keystroke. Calling draw() in the input loop was visibly
 * slow on large files — it re-lexes and re-renders every on-screen line for
 * each character typed — and that is what made the whole screen appear to
 * flicker while typing into a dialog. */
static int confirm(const char *title, const char *detail, int danger) {
    int yes = !danger;                       /* destructive → default No */
    draw();
    for (;;) {
        int tl = (int)strlen(title), dl = detail ? (int)strlen(detail) : 0;
        int bw = max2(max2(tl, dl) + 6, 34);
        bw = min2(bw, COLS - 2);
        int bh = detail ? 7 : 6;
        int y0 = max2(0, (LINES - bh) / 2), x0 = max2(0, (COLS - bw) / 2);
        int pair = danger ? CP_ERR : CP_STATUS;

        attron(COLOR_PAIR(pair));
        for (int r = 0; r < bh && y0 + r < LINES; r++) {
            move(y0 + r, x0);
            for (int i = 0; i < bw && x0 + i < COLS; i++) addch(' ');
        }
        attron(A_BOLD);
        mvaddnstr(y0 + 1, x0 + 2, title, bw - 4);
        attroff(A_BOLD);
        if (detail) mvaddnstr(y0 + 2, x0 + 2, detail, bw - 4);
        attroff(COLOR_PAIR(pair));

        /* buttons, right-aligned */
        int by = y0 + bh - 2, bx = x0 + bw - 20;
        for (int i = 0; i < 2; i++) {
            const char *lab = i ? "  Yes  " : "  No   ";
            int on = (i == yes);
            attron(COLOR_PAIR(on ? CP_SEL : pair));
            if (on) attron(A_BOLD | A_REVERSE);
            mvaddstr(by, bx + i * 9, lab);
            if (on) attroff(A_BOLD | A_REVERSE);
            attroff(COLOR_PAIR(on ? CP_SEL : pair));
        }
        curs_set(0);
        refresh();

        int c = read_key();
        switch (c) {
            case 'y': case 'Y':                     return 1;
            case 'n': case 'N': case 27:            return 0;
            case KEY_LEFT:  case MK(3, D_LEFT):     yes = 0; break;
            case KEY_RIGHT: case MK(3, D_RIGHT):    yes = 1; break;
            case '\t':                              yes = !yes; break;
            case '\r': case '\n': case KEY_ENTER:   return yes;
            case KEY_RESIZE:  draw();               break;  /* geometry moved */
        }
    }
}

/* ── text input dialog ────────────────────────────────────────────── */
/* Centered single-line editor. Returns 1 on Enter, 0 on Esc. */
static int input_box(const char *title, const char *hint, char *out, size_t cap) {
    size_t n = strlen(out);
    draw();                       /* the app behind the dialog, once */
    for (;;) {
        int bw = min2(max2((int)strlen(title) + 6, 46), COLS - 2);
        int bh = hint ? 6 : 5;
        int y0 = max2(0, (LINES - bh) / 2), x0 = max2(0, (COLS - bw) / 2);
        attron(COLOR_PAIR(CP_STATUS));
        for (int r = 0; r < bh && y0 + r < LINES; r++) {
            move(y0 + r, x0);
            for (int i = 0; i < bw && x0 + i < COLS; i++) addch(' ');
        }
        attron(A_BOLD);
        mvaddnstr(y0 + 1, x0 + 2, title, bw - 4);
        attroff(A_BOLD);
        if (hint) mvaddnstr(y0 + bh - 1, x0 + 2, hint, bw - 4);
        attroff(COLOR_PAIR(CP_STATUS));

        int fw = bw - 4;
        attron(COLOR_PAIR(CP_SEL));
        move(y0 + 3, x0 + 2);
        for (int i = 0; i < fw && x0 + 2 + i < COLS; i++) addch(' ');
        int off = max2(0, (int)n - fw + 1);
        mvaddnstr(y0 + 3, x0 + 2, out + off, fw);
        attroff(COLOR_PAIR(CP_SEL));
        curs_set(1);
        move(y0 + 3, x0 + 2 + min2((int)n - off, fw - 1));
        refresh();

        int c = read_key();
        if (c == 27) return 0;
        if (c == '\r' || c == '\n' || c == KEY_ENTER) return n > 0;
        if (c == KEY_RESIZE) { draw(); continue; }
        if (c == KEY_BACKSPACE || c == 127 || c == 8) { if (n) out[--n] = 0; }
        else if (c >= 32 && c < 256 && c != 127 && n + 1 < cap) {
            out[n++] = (char)c;
            out[n] = 0;
        }
    }
}

/* ── new file / folder in the tree ────────────────────────────────── */
/* insert `k` into `d`'s child list, keeping the dirs-first sort */
static void node_add_child(Node *d, Node *k) {
    d->kid = xrealloc(d->kid, (size_t)(d->nkid + 1) * sizeof(Node *));
    d->kid[d->nkid++] = k;
    qsort(d->kid, (size_t)d->nkid, sizeof(Node *), node_cmp);
}
static void tree_new_entry(void) {
    /* target dir = the selected folder, else the selected file's folder */
    Node *d = root;
    if (nvis) d = vis[tsel]->is_dir ? vis[tsel] : vis[tsel]->parent;
    if (!d) d = root;

    const char *shown = d == root ? "." : d->name;
    char title[NAME_MAX + 48];
    snprintf(title, sizeof title, "New entry in %s/", shown);

    char name[NAME_MAX + 1] = "";
    if (!input_box(title, "end with / to make a folder — Esc cancels",
                   name, sizeof name)) {
        set_msg("cancelled", NULL);
        return;
    }
    int is_dir = 0;
    size_t nl = strlen(name);
    while (nl && name[nl - 1] == '/') { is_dir = 1; name[--nl] = 0; }
    if (!nl) { set_msg("empty name", NULL); return; }
    if (strchr(name, '/') || !strcmp(name, ".") || !strcmp(name, "..")) {
        set_msg("invalid name: %s", name);
        return;
    }

    char path[PATH_MAX];
    if (snprintf(path, sizeof path, "%s/%s", d->path, name) >= (int)sizeof path) {
        set_msg("path too long", NULL);
        return;
    }
    /* load the folder BEFORE touching disk, or node_load() would pick the
     * new entry up too and we'd graft a duplicate below */
    if (!d->loaded) node_load(d);

    struct stat st;
    if (stat(path, &st) == 0) {
        if (!is_dir && S_ISREG(st.st_mode)) {      /* already there: just open */
            open_file(path);
            set_msg("already exists, opened %s", name);
            return;
        }
        set_msg("already exists: %s", name);
        return;
    }
    if (is_dir) {
        if (mkdir(path, 0755) != 0) { set_msg("could not create %s", name); return; }
    } else {
        FILE *f = fopen(path, "w");
        if (!f) { set_msg("could not create %s", name); return; }
        fclose(f);
    }

    /* graft into the tree without losing other folders' expanded state */
    Node *k = NULL;
    for (int i = 0; i < d->nkid; i++)
        if (!strcmp(d->kid[i]->name, name)) { k = d->kid[i]; break; }
    if (!k) {
        k = node_new(name, path, is_dir, d);
        if (is_dir) k->loaded = 1;
        node_add_child(d, k);
    }
    if (d != root) d->expanded = 1;
    tree_rebuild();
    for (int i = 0; i < nvis; i++) if (vis[i] == k) { tsel = i; break; }

    if (!is_dir) open_file(path);
    set_msg(is_dir ? "created folder %s" : "created %s", name);
}

/* ── delete from the tree ─────────────────────────────────────────── */
/* how many entries live under `dir` (capped — we only need "1 or many") */
static void count_tree(const char *dir, int *files, int *dirs, int depth) {
    if (depth > 16 || *files + *dirs > 5000) return;
    DIR *dp = opendir(dir);
    if (!dp) return;
    struct dirent *e;
    while ((e = readdir(dp))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char p[PATH_MAX];
        if (snprintf(p, sizeof p, "%s/%s", dir, e->d_name) >= (int)sizeof p) continue;
        struct stat st;
        if (lstat(p, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) { (*dirs)++; count_tree(p, files, dirs, depth + 1); }
        else (*files)++;
    }
    closedir(dp);
}
static int rm_rf(const char *path, int depth) {
    struct stat st;
    if (lstat(path, &st) != 0) return -1;
    if (!S_ISDIR(st.st_mode)) return unlink(path);
    if (depth > 16) return -1;
    DIR *dp = opendir(path);
    if (!dp) return -1;
    struct dirent *e;
    int rc = 0;
    while ((e = readdir(dp))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char p[PATH_MAX];
        if (snprintf(p, sizeof p, "%s/%s", path, e->d_name) >= (int)sizeof p) {
            rc = -1; continue;
        }
        if (rm_rf(p, depth + 1) != 0) rc = -1;
    }
    closedir(dp);
    return rmdir(path) != 0 ? -1 : rc;
}
static void node_unlink(Node *n) {               /* detach from parent */
    Node *p = n->parent;
    if (!p) return;
    for (int i = 0; i < p->nkid; i++)
        if (p->kid[i] == n) {
            memmove(p->kid + i, p->kid + i + 1,
                    (size_t)(p->nkid - i - 1) * sizeof(Node *));
            p->nkid--;
            break;
        }
    node_free(n);
}
/* close any tab whose file lived at (or under) `path` */
static void close_tabs_under(const char *path, int is_dir) {
    size_t pl = strlen(path);
    for (int i = ntabs - 1; i >= 0; i--) {
        if (tabs[i]->kind == TAB_TERM) continue;     /* terminals have no file */
        const char *tp = tabs[i]->path;
        int hit = is_dir ? (strncmp(tp, path, pl) == 0 && tp[pl] == '/')
                         : (strcmp(tp, path) == 0);
        if (hit) close_tab(i);
    }
}
static void tree_delete_selected(void) {
    if (nvis == 0) { set_msg("nothing selected", NULL); return; }
    Node *n = vis[tsel];

    char title[NAME_MAX + 64], detail[256];
    if (n->is_dir) {
        int files = 0, dirs = 0;
        count_tree(n->path, &files, &dirs, 0);
        snprintf(title, sizeof title, "Really delete folder \"%s\"?", n->name);
        if (files || dirs)
            snprintf(detail, sizeof detail,
                     "%d file(s) and %d folder(s) inside will be lost.", files, dirs);
        else
            snprintf(detail, sizeof detail, "The folder is empty.");
    } else {
        int open_dirty = 0;
        for (int i = 0; i < ntabs; i++)
            if (!strcmp(tabs[i]->path, n->path) && tabs[i]->dirty) open_dirty = 1;
        snprintf(title, sizeof title, "Really delete \"%s\"?", n->name);
        snprintf(detail, sizeof detail, "%s",
                 open_dirty ? "It is open with unsaved changes."
                            : "This cannot be undone.");
    }
    if (!confirm(title, detail, 1)) { set_msg("delete cancelled", NULL); return; }

    char path[PATH_MAX], name[NAME_MAX + 1];
    snprintf(path, sizeof path, "%s", n->path);
    snprintf(name, sizeof name, "%s", n->name);
    int is_dir = n->is_dir;

    if (rm_rf(path, 0) != 0) { set_msg("could not delete %s", name); return; }

    close_tabs_under(path, is_dir);
    node_unlink(n);
    tree_rebuild();
    if (tsel >= nvis) tsel = max2(0, nvis - 1);
    set_msg("deleted %s", name);
}

/* ── input ────────────────────────────────────────────────────────── */
/* Parse the tail of a CSI sequence ncurses didn't decode itself.
 * `defmod` is the modifier to assume when the sequence carries none:
 * 3 (Alt) if we saw a doubled ESC, 0 (plain) for a bare ESC [ ... .
 * Getting this right matters: a plain Up that ncurses failed to decode
 * must stay Up, not silently become Alt+Up.                            */
static int csi_tail(int defmod) {
    int ch, mod = 0, num = 0, first = 0, nnum = 0, final = 0;
    while ((ch = getch()) != ERR) {
        if (isdigit(ch)) num = num * 10 + (ch - '0');
        else if (ch == ';') { if (!nnum) first = num; nnum++; num = 0; }
        else { final = ch; if (nnum >= 1) mod = num; else first = num; break; }
    }
    if (mod < 2 || mod > 8) mod = defmod;
    int alt = (mod == 3 || mod == 4);
    if (final == '~') {
        if (first == 3) return alt ? K_ADEL : KEY_DC;    /* Delete */
        if (first == 2) return alt ? K_AINS : KEY_IC;    /* Insert */
        return K_NONE;
    }
    int dir;
    switch (final) {
        case 'A': dir = D_UP;    break;
        case 'B': dir = D_DOWN;  break;
        case 'D': dir = D_LEFT;  break;
        case 'C': dir = D_RIGHT; break;
        case 'H': dir = D_HOME;  break;
        case 'F': dir = D_END;   break;
        default:  return K_NONE;
    }
    if (mod >= 2) return MK(mod, dir);
    switch (dir) {                              /* unmodified: plain keys */
        case D_UP:    return KEY_UP;
        case D_DOWN:  return KEY_DOWN;
        case D_LEFT:  return KEY_LEFT;
        case D_RIGHT: return KEY_RIGHT;
        case D_HOME:  return KEY_HOME;
        default:      return KEY_END;
    }
}
/* How getch() should wait: -1 blocks, otherwise milliseconds. The main loop
 * switches to a short poll while a terminal tab is running; dialogs always
 * block, so they go through read_key() rather than read_key_raw(). */
static int g_timeout = -1;

static int read_key_raw(void) {
    int c = getch();
    if (c == KEY_SLEFT)  return MK(2, D_LEFT);
    if (c == KEY_SRIGHT) return MK(2, D_RIGHT);
    if (c == KEY_SHOME)  return MK(2, D_HOME);
    if (c == KEY_SEND)   return MK(2, D_END);
    if (c != 27) return c;
    nodelay(stdscr, TRUE);
    int c2 = getch(), r = 27;
    if      (c2 == ERR)       r = 27;
    else if (c2 == KEY_UP)    r = MK(3, D_UP);
    else if (c2 == KEY_DOWN)  r = MK(3, D_DOWN);
    else if (c2 == KEY_LEFT)  r = MK(3, D_LEFT);
    else if (c2 == KEY_RIGHT) r = MK(3, D_RIGHT);
    else if (c2 == KEY_DC)    r = K_ADEL;
    else if (c2 == KEY_IC)    r = K_AINS;
    else if (c2 == 27) {                     /* ESC ESC [ … = Alt+<key> */
        int c3 = getch();
        r = (c3 == '[' || c3 == 'O') ? csi_tail(3) : 27;
    }
    else if (c2 == '[' || c2 == 'O') r = csi_tail(0);   /* undecoded plain key */
    else if (c2 == '\r' || c2 == '\n' || c2 == KEY_ENTER) r = ALT('\n');
    else r = ALT(tolower(c2));
    timeout(g_timeout);            /* not nodelay(FALSE): that forces blocking */
    return r;
}
/* Blocking read, for dialogs and anything that owns the screen. */
static int read_key(void) {
    int save = g_timeout;
    g_timeout = -1;
    timeout(-1);
    int c = read_key_raw();
    g_timeout = save;
    timeout(save);
    return c;
}

/* ── clipboard ────────────────────────────────────────────────────── */
static void osc52_copy(const char *t, int len) {
    static const char b64[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if (len > 100000) return;
    char *out = xmalloc((size_t)len * 4 / 3 + 64);
    int n = 0;
    n += sprintf(out, "\033]52;c;");
    for (int i = 0; i < len; i += 3) {
        unsigned v = (unsigned char)t[i] << 16;
        if (i + 1 < len) v |= (unsigned char)t[i+1] << 8;
        if (i + 2 < len) v |= (unsigned char)t[i+2];
        out[n++] = b64[v >> 18 & 63];
        out[n++] = b64[v >> 12 & 63];
        out[n++] = i + 1 < len ? b64[v >> 6 & 63] : '=';
        out[n++] = i + 2 < len ? b64[v & 63] : '=';
    }
    out[n++] = '\a';
    ssize_t ignored = write(STDOUT_FILENO, out, (size_t)n);
    (void)ignored;
    free(out);
}
static void clip_set(char *t, int len) {       /* takes ownership */
    free(clip);
    clip = t; cliplen = len;
    osc52_copy(t, len);
}

/* ── movement ─────────────────────────────────────────────────────── */
/* Editing is byte-based, but the cursor must not land inside a multi-byte
 * character or a single arrow press would split it and the next edit would
 * corrupt the text. Continuation bytes are 10xxxxxx. */
static int utf8_cont(unsigned char c) { return (c & 0xc0) == 0x80; }
static int utf8_prev(Line *l, int cx) {
    if (cx <= 0) return 0;
    cx--;
    while (cx > 0 && utf8_cont((unsigned char)l->s[cx])) cx--;
    return cx;
}
static int utf8_next(Line *l, int cx) {
    if (cx >= l->len) return l->len;
    cx++;
    while (cx < l->len && utf8_cont((unsigned char)l->s[cx])) cx++;
    return cx;
}
enum { M_UP, M_DOWN, M_LEFT, M_RIGHT, M_HOME, M_END, M_PGUP, M_PGDN,
       M_WORDL, M_WORDR, M_DOCHOME, M_DOCEND };
static void word_left(Buf *b) {
    if (b->cx == 0) { if (b->cy > 0) { b->cy--; b->cx = b->ln[b->cy].len; } return; }
    Line *l = &b->ln[b->cy];
    int i = b->cx;
    while (i > 0 && !word_ch(l->s[i-1])) i--;
    while (i > 0 && word_ch(l->s[i-1])) i--;
    b->cx = i;
}
static void word_right(Buf *b) {
    Line *l = &b->ln[b->cy];
    if (b->cx >= l->len) { if (b->cy < b->n - 1) { b->cy++; b->cx = 0; } return; }
    int i = b->cx;
    while (i < l->len && !word_ch(l->s[i])) i++;
    while (i < l->len && word_ch(l->s[i])) i++;
    b->cx = i;
}
static void move_cursor(Buf *b, int kind, int shift) {
    if (shift && !b->sel) { b->sel = 1; b->ay = b->cy; b->ax = b->cx; }
    if (!shift) b->sel = 0;
    Line *l = &b->ln[b->cy];
    int page = LINES - 3;
    /* with wrap on, Up/Down step one *visual* row, like VS Code */
    int tw = g_wtw;
    if (wrap && tw > 0 && (kind == M_UP || kind == M_DOWN)) {
        int rx = rx_of(l, b->cx);
        int lastseg = line_rows(b, b->cy, tw) - 1;
        if (kind == M_DOWN) {
            if (rx / tw < lastseg)                          /* stay, next segment */
                b->cx = cx_of_rx(l, rx + tw);
            else if (b->cy < b->n - 1) {
                b->cy++;
                b->cx = cx_of_rx(&b->ln[b->cy], rx % tw);
            }
        } else {
            if (rx >= tw) b->cx = cx_of_rx(l, rx - tw);
            else if (b->cy > 0) {
                b->cy--;
                Line *p = &b->ln[b->cy];
                int last = (line_rows(b, b->cy, tw) - 1) * tw;
                b->cx = cx_of_rx(p, last + rx % tw);
            }
        }
        if (b->cx > b->ln[b->cy].len) b->cx = b->ln[b->cy].len;
        g_lastkind = AK_OTHER;
        return;
    }
    switch (kind) {
        case M_UP:    if (b->cy > 0) b->cy--; break;
        case M_DOWN:  if (b->cy < b->n - 1) b->cy++; break;
        case M_LEFT:
            if (b->cx > 0) b->cx = utf8_prev(l, b->cx);
            else if (b->cy > 0) { b->cy--; b->cx = b->ln[b->cy].len; }
            break;
        case M_RIGHT:
            if (b->cx < l->len) b->cx = utf8_next(l, b->cx);
            else if (b->cy < b->n - 1) { b->cy++; b->cx = 0; }
            break;
        case M_HOME: {                        /* smart home */
            int fw = 0;
            while (fw < l->len && isspace((unsigned char)l->s[fw])) fw++;
            b->cx = (b->cx == fw) ? 0 : fw;
            break;
        }
        case M_END:   b->cx = l->len; break;
        case M_PGUP:  b->cy = max2(0, b->cy - page); break;
        case M_PGDN:  b->cy = min2(b->n - 1, b->cy + page); break;
        case M_WORDL: word_left(b); break;
        case M_WORDR: word_right(b); break;
        case M_DOCHOME: b->cy = 0; b->cx = 0; break;
        case M_DOCEND:  b->cy = b->n - 1; b->cx = b->ln[b->cy].len; break;
    }
    if (b->cx > b->ln[b->cy].len) b->cx = b->ln[b->cy].len;
    g_lastkind = AK_OTHER;
}

/* ── editing ──────────────────────────────────────────────────────── */
static const char *indent_unit(Buf *b) {
    return b->lang->soft_tabs ? "    " : "\t";
}
static int line_indent_len(Line *l) {
    int i = 0;
    while (i < l->len && (l->s[i] == ' ' || l->s[i] == '\t')) i++;
    return i;
}
static void ed_type(Buf *b, int c) {
    begin_action(AK_TYPE);
    if (b->sel) { sel_delete(b); g_lastkind = AK_OTHER; }
    Line *l = &b->ln[b->cy];
    char ch = (char)c;
    /* skip over an identical auto-closable closing char */
    if (strchr(")]}\"'`", c) && b->cx < l->len && l->s[b->cx] == ch) {
        b->cx++;
        return;
    }
    /* dedent a lone '}' — eat one tab or up to tabstop spaces */
    if (c == '}' && line_indent_len(l) == b->cx && b->cx > 0) {
        int cut = 0;
        if (l->s[b->cx - 1] == '\t') cut = 1;
        else while (cut < tabstop && cut < b->cx && l->s[b->cx - 1 - cut] == ' ')
            cut++;
        if (cut) edit_del(b, b->cy, b->cx - cut, b->cy, b->cx);
    }
    /* auto-close pairs */
    const char *opens = "([{", *closes = ")]}";
    const char *p = strchr(opens, c);
    l = &b->ln[b->cy];
    int nextc = b->cx < l->len ? l->s[b->cx] : 0;
    int prevc = b->cx > 0 ? l->s[b->cx - 1] : 0;
    if (p && (!nextc || strchr(" \t)]}", nextc))) {
        char pair[3] = { ch, closes[p - opens], 0 };
        edit_ins(b, b->cy, b->cx, pair, 2);
        b->cx--;
        return;
    }
    if ((c == '"' || c == '\'' || c == '`') &&
        (!nextc || strchr(" \t)]}", nextc)) && !word_ch(prevc)) {
        char pair[3] = { ch, ch, 0 };
        edit_ins(b, b->cy, b->cx, pair, 2);
        b->cx--;
        return;
    }
    edit_ins(b, b->cy, b->cx, &ch, 1);
}
static void ed_enter(Buf *b) {
    begin_action(AK_OTHER);
    if (b->sel) sel_delete(b);
    Line *l = &b->ln[b->cy];
    int ind = min2(line_indent_len(l), b->cx);
    char prev = b->cx > 0 ? l->s[b->cx - 1] : 0;
    char next = b->cx < l->len ? l->s[b->cx] : 0;
    int deeper = prev && (strchr("([{", prev) ||
                          (b->lang->t1[0] /*python-ish*/ && prev == ':'));
    const char *u = indent_unit(b);
    char t[600];
    int n = 0;
    t[n++] = '\n';
    n += snprintf(t + n, sizeof t - (size_t)n, "%.*s", min2(ind, 256), l->s);
    if (deeper) n += snprintf(t + n, sizeof t - (size_t)n, "%s", u);
    int mid_y = -1, mid_x = -1;
    if (prev == '{' && next == '}') {         /* magic newline inside {} */
        mid_y = b->cy + 1;
        mid_x = n - 1;
        n += snprintf(t + n, sizeof t - (size_t)n, "\n%.*s", min2(ind, 256), l->s);
    }
    edit_ins(b, b->cy, b->cx, t, n);
    if (mid_y >= 0) { b->cy = mid_y; b->cx = ind + (int)strlen(u); (void)mid_x; }
}
static void ed_backspace(Buf *b) {
    if (b->sel) { begin_action(AK_OTHER); sel_delete(b); return; }
    begin_action(AK_BS);
    Line *l = &b->ln[b->cy];
    if (b->cx > 0) {
        /* delete both halves of an empty auto-closed pair */
        if (b->cx < l->len) {
            char a = l->s[b->cx - 1], z = l->s[b->cx];
            if ((a == '(' && z == ')') || (a == '[' && z == ']') ||
                (a == '{' && z == '}') ||
                ((a == '"' || a == '\'' || a == '`') && z == a)) {
                edit_del(b, b->cy, b->cx - 1, b->cy, b->cx + 1);
                return;
            }
        }
        edit_del(b, b->cy, utf8_prev(l, b->cx), b->cy, b->cx);
    } else if (b->cy > 0) {
        edit_del(b, b->cy - 1, b->ln[b->cy - 1].len, b->cy, 0);
    }
}
static void ed_delete(Buf *b) {
    if (b->sel) { begin_action(AK_OTHER); sel_delete(b); return; }
    begin_action(AK_OTHER);
    Line *l = &b->ln[b->cy];
    if (b->cx < l->len) edit_del(b, b->cy, b->cx, b->cy, utf8_next(l, b->cx));
    else if (b->cy < b->n - 1) edit_del(b, b->cy, b->cx, b->cy + 1, 0);
}
static void ed_tab(Buf *b, int dedent) {
    begin_action(AK_OTHER);
    int y1, x1, y2, x2;
    const char *u = indent_unit(b);
    int ul = (int)strlen(u);
    /* sel_norm() leaves the coords untouched when it returns 0 (which
     * includes an active-but-empty selection), so seed them first and
     * branch on its result rather than on b->sel. */
    int had_sel = sel_norm(b, &y1, &x1, &y2, &x2);
    if (!had_sel) { y1 = y2 = b->cy; x1 = x2 = b->cx; b->sel = 0; }
    if (had_sel || dedent) {
        if (had_sel && x2 == 0 && y2 > y1) y2--;   /* don't touch empty tail */
        for (int y = y1; y <= y2; y++) {
            Line *l = &b->ln[y];
            if (dedent) {
                int cut = 0;
                if (l->len && l->s[0] == '\t') cut = 1;
                else while (cut < ul && cut < l->len && l->s[cut] == ' ') cut++;
                if (cut) edit_del(b, y, 0, y, cut);
            } else if (l->len) {
                edit_ins(b, y, 0, u, ul);
            }
        }
        if (had_sel) { b->ay = y1; b->ax = 0; b->cy = y2; b->cx = b->ln[y2].len; }
        else { b->cy = y1; b->cx = min2(b->cx, b->ln[y1].len); }
        return;
    }
    if (b->lang->soft_tabs) {
        int col = rx_of(&b->ln[b->cy], b->cx);
        int k = tabstop - col % tabstop;
        edit_ins(b, b->cy, b->cx, "        ", k);
    } else edit_ins(b, b->cy, b->cx, "\t", 1);
}
static void ed_dup_line(Buf *b) {
    begin_action(AK_OTHER);
    int y1 = b->cy, y2 = b->cy, x1, x2;
    sel_norm(b, &y1, &x1, &y2, &x2);   /* selection => duplicate whole block */
    int tlen;
    char *t = range_text(b, y1, 0, y2, b->ln[y2].len, &tlen);
    char *t2 = xmalloc((size_t)tlen + 2);
    t2[0] = '\n';
    memcpy(t2 + 1, t, (size_t)tlen + 1);
    free(t);
    int savecx = b->cx;
    edit_ins(b, y2, b->ln[y2].len, t2, tlen + 1);
    free(t2);
    b->cy = min2(y2 + (y2 - y1) + 1, b->n - 1);
    b->cx = min2(savecx, b->ln[b->cy].len);
    b->sel = 0;
}
static void ed_del_line(Buf *b) {
    begin_action(AK_OTHER);
    int y1 = b->cy, y2 = b->cy, x1, x2;
    sel_norm(b, &y1, &x1, &y2, &x2);
    b->sel = 0;
    if (y2 < b->n - 1) edit_del(b, y1, 0, y2 + 1, 0);
    else if (y1 > 0)   edit_del(b, y1 - 1, b->ln[y1 - 1].len, y2, b->ln[y2].len);
    else               edit_del(b, 0, 0, y2, b->ln[y2].len);
    b->cx = min2(b->cx, b->ln[b->cy].len);
}
static void ed_move_lines(Buf *b, int down) {
    int y1 = b->cy, y2 = b->cy, x1, x2;
    int had_sel = sel_norm(b, &y1, &x1, &y2, &x2);
    if (had_sel && x2 == 0 && y2 > y1) y2--;
    if ((!down && y1 == 0) || (down && y2 >= b->n - 1)) return;
    begin_action(AK_OTHER);
    int savecx = b->cx;
    int tlen;
    char *t = range_text(b, y1, 0, y2, b->ln[y2].len, &tlen);
    /* remove block (with one newline) */
    if (y2 < b->n - 1) edit_del(b, y1, 0, y2 + 1, 0);
    else               edit_del(b, y1 - 1, b->ln[y1 - 1].len, y2, b->ln[y2].len);
    int ny = down ? y1 + 1 : y1 - 1;
    if (ny >= b->n) {                          /* append at very end */
        char *t2 = xmalloc((size_t)tlen + 2);
        t2[0] = '\n'; memcpy(t2 + 1, t, (size_t)tlen + 1);
        edit_ins(b, b->n - 1, b->ln[b->n - 1].len, t2, tlen + 1);
        free(t2);
        ny = b->n - (y2 - y1 + 1);
    } else {
        char *t2 = xmalloc((size_t)tlen + 2);
        memcpy(t2, t, (size_t)tlen);
        t2[tlen] = '\n'; t2[tlen + 1] = 0;
        edit_ins(b, ny, 0, t2, tlen + 1);
        free(t2);
    }
    free(t);
    int nlines = y2 - y1;
    if (had_sel) {
        b->sel = 1; b->ay = ny; b->ax = 0;
        b->cy = ny + nlines; b->cx = b->ln[b->cy].len;
    } else {
        b->cy = ny; b->cx = min2(savecx, b->ln[ny].len);
    }
}
static void ed_toggle_comment(Buf *b) {
    const char *tok = b->lang->lc[0] ? b->lang->lc : NULL;
    if (!tok) { set_msg("no line comment for %s", b->lang->name); return; }
    int tl = (int)strlen(tok);
    begin_action(AK_OTHER);
    int y1 = b->cy, y2 = b->cy, x1, x2;
    int had_sel = sel_norm(b, &y1, &x1, &y2, &x2);
    if (had_sel && x2 == 0 && y2 > y1) y2--;
    /* all non-empty lines commented? */
    int all = 1, any = 0;
    for (int y = y1; y <= y2; y++) {
        Line *l = &b->ln[y];
        int i = line_indent_len(l);
        if (i >= l->len) continue;
        any = 1;
        if (l->len - i < tl || memcmp(l->s + i, tok, (size_t)tl) != 0) all = 0;
    }
    if (!any) return;
    for (int y = y1; y <= y2; y++) {
        Line *l = &b->ln[y];
        int i = line_indent_len(l);
        if (i >= l->len) continue;
        if (all) {
            int cut = tl;
            if (i + cut < l->len && l->s[i + cut] == ' ') cut++;
            edit_del(b, y, i, y, i + cut);
        } else {
            char t[16];
            snprintf(t, sizeof t, "%s ", tok);
            edit_ins(b, y, i, t, tl + 1);
        }
    }
    if (had_sel) { b->sel = 1; b->ay = y1; b->ax = 0; b->cy = y2; b->cx = b->ln[y2].len; }
    else { b->cy = y1; b->cx = min2(b->cx, b->ln[y1].len); }
}
static void ed_copy(Buf *b, int cut) {
    int y1, x1, y2, x2;
    if (sel_norm(b, &y1, &x1, &y2, &x2)) {
        int tlen;
        char *t = range_text(b, y1, x1, y2, x2, &tlen);
        clip_set(t, tlen);
        if (cut) { begin_action(AK_OTHER); sel_delete(b); }
        set_msg(cut ? "cut selection" : "copied selection", NULL);
    } else {                                    /* whole line, VS Code style */
        int tlen;
        char *t = range_text(b, b->cy, 0, b->cy, b->ln[b->cy].len, &tlen);
        char *t2 = xmalloc((size_t)tlen + 2);
        memcpy(t2, t, (size_t)tlen);
        t2[tlen] = '\n'; t2[tlen + 1] = 0;
        free(t);
        clip_set(t2, tlen + 1);
        if (cut) ed_del_line(b);
        set_msg(cut ? "cut line" : "copied line", NULL);
    }
}
static void ed_paste_text(Buf *b, const char *t, int len) {
    begin_action(AK_OTHER);
    if (b->sel) sel_delete(b);
    edit_ins(b, b->cy, b->cx, t, len);
}
static void ed_open_below(Buf *b) {
    b->sel = 0;
    b->cx = b->ln[b->cy].len;
    ed_enter(b);
}

/* ── prompt ───────────────────────────────────────────────────────── */
/* ── command history ──────────────────────────────────────────────── */
#define HIST_MAX 200
static char **rhist = NULL;
static int    nrhist = 0;

static void hist_path(char *out, size_t cap) {
    const char *home = getenv("HOME");
    snprintf(out, cap, "%s/.sds_history", home && *home ? home : ".");
}
static void hist_add(const char *s) {
    if (!s || !*s) return;
    if (nrhist && !strcmp(rhist[nrhist - 1], s)) return;   /* no dupes in a row */
    if (nrhist == HIST_MAX) {
        free(rhist[0]);
        memmove(rhist, rhist + 1, (size_t)(nrhist - 1) * sizeof(char *));
        nrhist--;
    }
    rhist = xrealloc(rhist, (size_t)(nrhist + 1) * sizeof(char *));
    rhist[nrhist++] = xstrdup(s);
}
static void hist_load(void) {
    char p[PATH_MAX];
    hist_path(p, sizeof p);
    FILE *f = fopen(p, "r");
    if (!f) return;
    char *ln = NULL; size_t cap = 0; ssize_t r;
    while ((r = getline(&ln, &cap, f)) != -1) {
        while (r > 0 && (ln[r-1] == '\n' || ln[r-1] == '\r')) ln[--r] = 0;
        if (r) hist_add(ln);
    }
    free(ln);
    fclose(f);
}
static void hist_save(void) {
    if (!nrhist) return;
    char p[PATH_MAX];
    hist_path(p, sizeof p);
    FILE *f = fopen(p, "w");
    if (!f) return;
    for (int i = 0; i < nrhist; i++) fprintf(f, "%s\n", rhist[i]);
    fclose(f);
}

/* Status-bar line editor. `use_hist` enables Up/Down recall of past
 * commands; the half-typed line is kept as a draft below the newest entry. */
static int prompt(const char *label, char *out, size_t cap,
                  void (*live)(const char *), int use_hist) {
    size_t n = strlen(out);
    int hidx = nrhist;
    char draft[512] = "";
    draw();                       /* the app behind the prompt, once */
    for (;;) {
        /* only the prompt line is repainted per keystroke; the editor behind
         * it is redrawn solely when a live callback changes its highlighting */
        attron(COLOR_PAIR(CP_STATUS) | A_BOLD);
        move(LINES - 1, 0);
        for (int i = 0; i < COLS; i++) addch(' ');
        mvprintw(LINES - 1, 0, " %s%s", label, out);
        attroff(COLOR_PAIR(CP_STATUS) | A_BOLD);
        curs_set(1);
        refresh();
        int c = read_key();
        if (c == 27) return 0;
        if (c == '\r' || c == '\n' || c == KEY_ENTER) return 1;
        if (c == KEY_RESIZE) { draw(); continue; }
        if (use_hist && (c == KEY_UP || c == KEY_DOWN)) {
            if (c == KEY_UP && hidx > 0) {
                if (hidx == nrhist) snprintf(draft, sizeof draft, "%s", out);
                snprintf(out, cap, "%s", rhist[--hidx]);
            } else if (c == KEY_DOWN && hidx < nrhist) {
                if (++hidx == nrhist) snprintf(out, cap, "%s", draft);
                else                  snprintf(out, cap, "%s", rhist[hidx]);
            }
            n = strlen(out);
            continue;
        }
        if (c == KEY_BACKSPACE || c == 127 || c == 8) {
            if (n) out[--n] = 0;
        } else if (c >= 32 && c < 256 && c != 127 && n + 1 < cap) {
            out[n++] = (char)c;
            out[n] = 0;
        } else continue;
        if (live) { live(out); draw(); }
    }
}

/* ── find / replace / goto ────────────────────────────────────────── */
static int find_from(Buf *b, int y, int x, int *my, int *mx) {
    int q = (int)strlen(findq);
    if (!q) return 0;
    for (int k = 0; k <= b->n; k++) {
        int yy = (y + k) % b->n;
        int from = (k == 0) ? x : 0;
        int at = ci_find(b->ln[yy].s, b->ln[yy].len, findq, q, from);
        if (at >= 0) { *my = yy; *mx = at; return 1; }
    }
    return 0;
}
static void find_live(const char *q) {
    (void)q;
    if (cur < 0) return;
    Buf *b = tabs[cur];
    int my, mx;
    if (find_from(b, b->ay, b->ax, &my, &mx)) {      /* ay/ax reused as anchor */
        b->cy = my; b->cx = mx;
    }
}
static void do_find(void) {
    if (cur < 0) return;
    Buf *b = tabs[cur];
    b->sel = 0;
    b->ay = b->cy; b->ax = b->cx;      /* search anchor */
    find_show = 1;
    for (;;) {
        int r = prompt("Find: ", findq, sizeof findq, find_live, 0);
        if (!r) break;                                 /* Esc: stay put */
        int my, mx;                                    /* Enter: next   */
        if (find_from(b, b->cy, b->cx + 1, &my, &mx)) {
            b->cy = my; b->cx = mx;
            b->ay = my; b->ax = mx;
        } else { set_msg("no match: %s", findq); break; }
    }
}
static void find_next(void) {
    if (cur < 0 || !findq[0]) return;
    Buf *b = tabs[cur];
    int my, mx;
    find_show = 1;
    if (find_from(b, b->cy, b->cx + 1, &my, &mx)) { b->cy = my; b->cx = mx; }
    else set_msg("no match: %s", findq);
}
static void do_replace(void) {
    if (cur < 0) return;
    Buf *b = tabs[cur];
    static char rep[256] = "";
    b->ay = b->cy; b->ax = b->cx;
    find_show = 1;
    if (!prompt("Replace: ", findq, sizeof findq, find_live, 0)) return;
    if (!findq[0]) return;
    if (!prompt("With: ", rep, sizeof rep, NULL, 0)) return;
    int q = (int)strlen(findq), rl = (int)strlen(rep);
    int y = b->cy, x = b->cx, done = 0, all = 0, count = 0;
    int wrapped_y = y, wrapped_x = x, first = 1;
    begin_action(AK_OTHER);
    while (!done) {
        int my, mx;
        if (!find_from(b, y, x, &my, &mx)) break;
        if (!first && my == wrapped_y && mx == wrapped_x) break;
        if (first) { wrapped_y = my; wrapped_x = mx; first = 0; }
        b->cy = my; b->cx = mx;
        int act = 'y';
        if (!all) {
            draw();
            attron(COLOR_PAIR(CP_STATUS) | A_BOLD);
            mvprintw(LINES - 1, 0, " replace? y=yes n=skip a=all q=done ");
            attroff(COLOR_PAIR(CP_STATUS) | A_BOLD);
            refresh();
            act = read_key();
        }
        if (act == 'q' || act == 27) break;
        if (act == 'a') { all = 1; act = 'y'; }
        if (act == 'y') {
            edit_del(b, my, mx, my, mx + q);
            if (rl) edit_ins(b, my, mx, rep, rl);
            count++;
            y = my; x = mx + rl;
        } else { y = my; x = mx + 1; }
    }
    char cnt[32];
    snprintf(cnt, sizeof cnt, "%d", count);
    set_msg("replaced %s occurrence(s)", cnt);
}
static void do_goto(void) {
    if (cur < 0) return;
    char in[16] = "";
    if (!prompt("Line: ", in, sizeof in, NULL, 0)) return;
    int ln = atoi(in);
    if (ln < 1) return;
    Buf *b = tabs[cur];
    b->cy = min2(ln - 1, b->n - 1);
    b->cx = 0;
    b->sel = 0;
}

/* ── quick open (Ctrl+P) ──────────────────────────────────────────── */
static char **qofiles = NULL;
static int    nqo = 0;
static void qo_walk(const char *dir, const char *rel, int depth) {
    if (depth > 12 || nqo >= QO_MAX) return;
    DIR *dp = opendir(dir);
    if (!dp) return;
    struct dirent *e;
    while ((e = readdir(dp)) && nqo < QO_MAX) {
        if (e->d_name[0] == '.') continue;
        if (!strcmp(e->d_name, "node_modules") || !strcmp(e->d_name, "target") ||
            !strcmp(e->d_name, "__pycache__") || !strcmp(e->d_name, "build") ||
            !strcmp(e->d_name, "dist") || !strcmp(e->d_name, "venv")) continue;
        char p[PATH_MAX], r[PATH_MAX];
        if (snprintf(p, sizeof p, "%s/%s", dir, e->d_name) >= (int)sizeof p) continue;
        if (snprintf(r, sizeof r, "%s%s%s", rel, rel[0] ? "/" : "", e->d_name)
            >= (int)sizeof r) continue;
        struct stat st;
        if (stat(p, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) qo_walk(p, r, depth + 1);
        else {
            qofiles = xrealloc(qofiles, (size_t)(nqo + 1) * sizeof(char *));
            qofiles[nqo++] = xstrdup(r);
        }
    }
    closedir(dp);
}
static int fuzzy_score(const char *hay, const char *nee) {
    if (!nee[0]) return 1;
    int hl = (int)strlen(hay), nl = (int)strlen(nee);
    int at = ci_find(hay, hl, nee, nl, 0);
    if (at >= 0) return 1000 - at;               /* substring: best */
    int hi = 0, gaps = 0, last = -1;
    for (int ni = 0; ni < nl; ni++) {
        while (hi < hl &&
               tolower((unsigned char)hay[hi]) != tolower((unsigned char)nee[ni]))
            hi++;
        if (hi == hl) return 0;
        if (last >= 0) gaps += hi - last - 1;
        last = hi++;
    }
    return max2(1, 500 - gaps);
}
static void do_quickopen(void) {
    for (int i = 0; i < nqo; i++) free(qofiles[i]);
    free(qofiles); qofiles = NULL; nqo = 0;
    qo_walk(root->path, "", 0);
    char q[128] = "";
    int seln = 0;
    int *idx = xmalloc((size_t)max2(nqo, 1) * sizeof(int));
    int *scr = xmalloc((size_t)max2(nqo, 1) * sizeof(int));
    draw();                       /* the app behind the picker, once */
    for (;;) {
        int nm = 0;
        for (int i = 0; i < nqo; i++) {
            int s = fuzzy_score(qofiles[i], q);
            if (s > 0) { idx[nm] = i; scr[nm] = s; nm++; }
        }
        for (int i = 1; i < nm; i++) {           /* insertion sort by score */
            int ii = idx[i], ss = scr[i], j = i - 1;
            while (j >= 0 && scr[j] < ss) { idx[j+1] = idx[j]; scr[j+1] = scr[j]; j--; }
            idx[j+1] = ii; scr[j+1] = ss;
        }
        if (seln >= nm) seln = max2(0, nm - 1);
        int bw = min2(COLS - 4, 64), y0 = 2;
        /* fixed height: the panel neither jumps as you type nor leaves stale
         * rows behind, which matters now that the app is not redrawn under it */
        int lh = min2(12, max2(LINES - y0 - 3, 1));
        int x0 = (COLS - bw) / 2;
        attron(COLOR_PAIR(CP_STATUS));
        move(y0, x0);
        for (int i = 0; i < bw; i++) addch(' ');
        mvprintw(y0, x0, " > %s", q);
        attroff(COLOR_PAIR(CP_STATUS));
        for (int r = 0; r < lh; r++) {
            move(y0 + 1 + r, x0);
            int pair = (r == seln) ? CP_SEL : CP_TAB;
            attron(COLOR_PAIR(pair));
            for (int i = 0; i < bw; i++) addch(' ');
            if (r < nm) mvaddnstr(y0 + 1 + r, x0 + 1, qofiles[idx[r]], bw - 2);
            attroff(COLOR_PAIR(pair));
        }
        curs_set(1);
        move(y0, x0 + 3 + (int)strlen(q));
        refresh();
        int c = read_key();
        if (c == 27) break;
        if (c == KEY_RESIZE) { draw(); continue; }
        if (c == KEY_UP) { if (seln > 0) seln--; continue; }
        if (c == KEY_DOWN) { if (seln < min2(nm, lh) - 1) seln++; continue; }
        if (c == '\r' || c == '\n' || c == KEY_ENTER) {
            if (nm) {
                char p[PATH_MAX];
                snprintf(p, sizeof p, "%s/%s", root->path, qofiles[idx[seln]]);
                open_file(p);
            }
            break;
        }
        if (c == KEY_BACKSPACE || c == 127 || c == 8) {
            size_t n = strlen(q);
            if (n) q[n - 1] = 0;
            seln = 0;
        } else if (c >= 32 && c < 256 && strlen(q) + 1 < sizeof q) {
            size_t n = strlen(q);
            q[n] = (char)c; q[n + 1] = 0;
            seln = 0;
        }
    }
    free(idx); free(scr);
}

/* ── autocomplete (Ctrl+Space) ────────────────────────────────────── */
static void collect_words(char ***out, int *nout, const char *prefix) {
    int plen = (int)strlen(prefix);
    char **w = NULL;
    int nw = 0, capw = 0;
    /* words from all open buffers + current language keywords */
    for (int t = 0; t < ntabs; t++) {
        Buf *b = tabs[t];
        for (int y = 0; y < b->n; y++) {
            Line *l = &b->ln[y];
            int i = 0;
            while (i < l->len) {
                if (word_ch(l->s[i]) && !isdigit((unsigned char)l->s[i])) {
                    int j = i;
                    while (j < l->len && word_ch(l->s[j])) j++;
                    int wl = j - i;
                    if (wl >= 3 && wl < 64 && wl > plen &&
                        strncasecmp(l->s + i, prefix, (size_t)plen) == 0) {
                        char tmp[64];
                        memcpy(tmp, l->s + i, (size_t)wl);
                        tmp[wl] = 0;
                        int dup = 0;
                        for (int k = 0; k < nw; k++)
                            if (!strcmp(w[k], tmp)) { dup = 1; break; }
                        if (!dup) {
                            if (nw == capw) {
                                capw = capw ? capw * 2 : 64;
                                w = xrealloc(w, (size_t)capw * sizeof(char *));
                            }
                            w[nw++] = xstrdup(tmp);
                        }
                    }
                    i = j;
                } else i++;
                if (nw >= 500) break;
            }
            if (nw >= 500) break;
        }
    }
    *out = w; *nout = nw;
}
static int str_cmp(const void *a, const void *b) {
    return strcasecmp(*(char * const *)a, *(char * const *)b);
}
static void do_complete(void) {
    if (cur < 0) return;
    Buf *b = tabs[cur];
    Line *l = &b->ln[b->cy];
    int s = b->cx;
    while (s > 0 && word_ch(l->s[s - 1])) s--;
    if (s == b->cx) { set_msg("nothing to complete", NULL); return; }
    char prefix[64];
    int plen = min2(b->cx - s, 63);
    memcpy(prefix, l->s + s, (size_t)plen);
    prefix[plen] = 0;
    char **w; int nw;
    collect_words(&w, &nw, prefix);
    if (!nw) { set_msg("no completions for %s", prefix); return; }
    qsort(w, (size_t)nw, sizeof(char *), str_cmp);
    int seln = 0;
    draw();                       /* the app behind the list, once; the list
                                   * is a fixed size, only the highlight moves */
    for (;;) {
        int show = min2(nw, 8);
        int py = 2 + (b->cy - b->rowoff);
        int px = tree_hidden ? 1 : tree_w + 2;
        if (py + show >= LINES - 1) py = max2(1, py - show - 1);
        int bw = 24;
        for (int i = 0; i < show; i++)
            bw = max2(bw, (int)strlen(w[i]) + 2);
        for (int r = 0; r < show; r++) {
            int pair = (r == seln) ? CP_SEL : CP_TAB;
            attron(COLOR_PAIR(pair));
            move(py + r, px);
            for (int i = 0; i < bw && px + i < COLS; i++) addch(' ');
            mvaddnstr(py + r, px + 1, w[r], min2(bw - 2, COLS - px - 1));
            attroff(COLOR_PAIR(pair));
        }
        refresh();
        int c = read_key();
        if (c == KEY_UP)   { if (seln > 0) seln--; continue; }
        if (c == KEY_DOWN) { if (seln < show - 1) seln++; continue; }
        if (c == '\r' || c == '\n' || c == KEY_ENTER || c == '\t') {
            const char *word = w[seln];
            begin_action(AK_OTHER);
            edit_ins(b, b->cy, b->cx, word + plen, (int)strlen(word) - plen);
            break;
        }
        break;                                     /* any other key cancels */
    }
    for (int i = 0; i < nw; i++) free(w[i]);
    free(w);
}

/* ── bracketed paste ──────────────────────────────────────────────── */
static void handle_bracketed_paste(void) {
    size_t cap = 256, n = 0;
    char *t = xmalloc(cap);
    for (;;) {
        int c = getch();
        if (c == K_PEND || c == ERR) break;
        if (c == '\r') c = '\n';
        if (c > 255) continue;
        if (n + 1 >= cap) { cap *= 2; t = xrealloc(t, cap); }
        t[n++] = (char)c;
    }
    t[n] = 0;
    if (cur >= 0 && n) ed_paste_text(tabs[cur], t, (int)n);
    free(t);
}

/* ── run command ──────────────────────────────────────────────────── */
static void run_command(void) {
    char in[512] = "";              /* always start empty; Up recalls history */
    if (!prompt("Run: ", in, sizeof in, NULL, 1)) return;
    if (!in[0]) return;
    hist_add(in);
    hist_save();

    int saved = 0;                    /* compile what's on screen, not on disk */
    for (int i = 0; i < ntabs; i++)
        if (tabs[i]->dirty && buf_save(tabs[i]) == 0) saved++;

    def_prog_mode();
    tc_restore();                       /* the command gets the real palette */
    endwin();
    printf("\033[?2004l");
    printf("\033[H\033[2J\033[3J");     /* clear screen + scrollback */
    if (saved) printf("[saved %d file(s)]\n", saved);
    printf("$ %s\n", in);
    fflush(stdout);

    int st = system(in);
    /* system() hands back a wait status, not an exit code */
    if (st == -1)              printf("\n[could not run]");
    else if (WIFSIGNALED(st))  printf("\n\033[31m[killed by signal %d]\033[0m",
                                      WTERMSIG(st));
    else if (WEXITSTATUS(st))  printf("\n\033[31m[exit %d]\033[0m", WEXITSTATUS(st));
    else                       printf("\n\033[32m[exit 0]\033[0m");
    printf(" — press any key ");
    fflush(stdout);

    reset_prog_mode();
    apply_theme();                    /* and sds takes its palette back */
    printf("\033[?2004h");
    fflush(stdout);
    getch();                          /* any key, not just Enter */

    tree_refresh();                   /* pick up a.out, build/, generated files */
    refresh();
}

/* ── app actions ──────────────────────────────────────────────────── */
static void act_save(void) {
    if (cur < 0) return;
    if (tabs[cur]->kind != TAB_FILE) { set_msg("nothing to save here", NULL); return; }
    if (buf_save(tabs[cur]) == 0) {
        set_msg("saved %s", tabs[cur]->name);
        git_refresh();                     /* the file's status just changed */
    } else set_msg("save failed: %s", tabs[cur]->path);
}
static void act_close(void) {
    if (cur < 0) return;
    if (tabs[cur]->kind == TAB_TERM) {
        /* a still-running shell is worth one confirmation */
        Term *t = tabs[cur]->term;
        if (t && !t->dead && !pending_close) {
            pending_close = 1;
            set_msg("shell still running — Alt+W again to kill it", NULL);
            return;
        }
        close_tab(cur);
        pending_close = 0;
        return;
    }
    if (tabs[cur]->dirty && !pending_close) {
        pending_close = 1;
        set_msg("unsaved changes — Alt+W again to discard", NULL);
        return;
    }
    close_tab(cur);
    pending_close = 0;
}
static int act_quit(void) {
    int dirty = 0, shells = 0;
    for (int i = 0; i < ntabs; i++) {
        if (tabs[i]->kind == TAB_TERM) {
            if (tabs[i]->term && !tabs[i]->term->dead) shells++;
        } else dirty |= tabs[i]->dirty;
    }
    if ((dirty || shells) && !pending_quit) {
        pending_quit = 1;
        if (dirty) set_msg("unsaved changes — Alt+Q again to quit anyway", NULL);
        else       set_msg("shells still running — Alt+Q again to quit", NULL);
        return 0;
    }
    return 1;
}

/* ── main ─────────────────────────────────────────────────────────── */
/* Build a tree-sitter grammar from its upstream repo and install it, with the
 * matching highlight query, where ts_for() looks. Needs git and a compiler.
 * Grammars are not packaged consistently across distros — on Arch there is no
 * tree-sitter-cpp package at all — so sds ships this rather than assume. */
static int fetch_grammar(const char *lang) {
#ifndef SDS_TREESITTER
    (void)lang;
    fprintf(stderr, "sds: built without tree-sitter support\n"
                    "     rebuild with -DSDS_TREESITTER (see install.sh)\n");
    return 1;
#else
    char gdir[PATH_MAX - 64], qdir[PATH_MAX - 64], tmp[256], cmd[PATH_MAX * 8];
    for (const char *p = lang; *p; p++)
        if (!isalnum((unsigned char)*p) && *p != '_') {
            fprintf(stderr, "sds: bad grammar name: %s\n", lang);
            return 1;
        }
    ts_data_dir(gdir, sizeof gdir, "grammars");
    ts_data_dir(qdir, sizeof qdir, "queries");
    snprintf(tmp, sizeof tmp, "/tmp/sds-grammar-%s.%d", lang, (int)getpid());

    snprintf(cmd, sizeof cmd,
        "set -e\n"
        "mkdir -p '%s' '%s/%s'\n"
        "rm -rf '%s'\n"
        "git clone --depth 1 -q https://github.com/tree-sitter/tree-sitter-%s '%s'\n"
        "cd '%s'\n"
        "src=src\n"
        "[ -d bindings ] || true\n"
        "cc -O2 -fPIC -shared -I \"$src\" -o '%s/libtree-sitter-%s.so' \\\n"
        "   \"$src\"/parser.c $([ -f \"$src/scanner.c\" ] && echo \"$src/scanner.c\")\n"
        "cp queries/highlights.scm '%s/%s/highlights.scm'\n"
        "cd / && rm -rf '%s'\n",
        gdir, qdir, lang, tmp, lang, tmp, tmp, gdir, lang, qdir, lang, tmp);

    printf("fetching tree-sitter-%s …\n", lang);
    int st = system(cmd);
    if (st != 0) {
        fprintf(stderr, "sds: could not install grammar for %s\n", lang);
        return 1;
    }
    printf("installed %s/libtree-sitter-%s.so\n", gdir, lang);
    printf("installed %s/%s/highlights.scm\n", qdir, lang);

    /* A language whose query inherits another needs that one's rules too,
     * or e.g. C++ loses all its primitive-type highlighting. */
    const char *par = ts_parent_of(lang);
    if (par) {
        char pq[PATH_MAX];
        snprintf(pq, sizeof pq, "%s/%s/highlights.scm", qdir, par);
        if (access(pq, F_OK) != 0) {
            printf("%s inherits %s — fetching its query too\n", lang, par);
            snprintf(cmd, sizeof cmd,
                "set -e\n"
                "mkdir -p '%s/%s'\n"
                "rm -rf '%s'\n"
                "git clone --depth 1 -q "
                "https://github.com/tree-sitter/tree-sitter-%s '%s'\n"
                "cp '%s/queries/highlights.scm' '%s/%s/highlights.scm'\n"
                "rm -rf '%s'\n",
                qdir, par, tmp, par, tmp, tmp, qdir, par, tmp);
            if (system(cmd) == 0) printf("installed %s/%s/highlights.scm\n", qdir, par);
            else fprintf(stderr, "sds: warning: could not fetch %s query\n", par);
        }
    }
    return 0;
#endif
}

int main(int argc, char **argv) {
    setlocale(LC_ALL, "");
    if (argc > 1 && !strcmp(argv[1], "--fetch-grammar")) {
        if (argc < 3) { fprintf(stderr, "usage: sds --fetch-grammar <lang>\n"); return 1; }
        return fetch_grammar(argv[2]);
    }
    /* Dump a PDF's text and exit — the same extraction the viewer shows, but
     * pipeable, and the handle to debug a document that renders oddly. */
    if (argc > 1 && !strcmp(argv[1], "--pdf-text")) {
        if (argc < 3) { fprintf(stderr, "usage: sds --pdf-text <file.pdf>\n"); return 1; }
        Buf *b = pdf_load(argv[2]);
        if (!b) { fprintf(stderr, "sds: can't read %s\n", argv[2]); return 1; }
        for (int pg = 0; pg < b->pdf->npg; pg++) {
            if (pg) pdf_page_into(b, pg);
            printf("=== page %d/%d ===\n", pg + 1, b->pdf->npg);
            for (int i = 0; i < b->n; i++)
                printf("%.*s\n", b->ln[i].len, b->ln[i].s);
        }
        buf_free(b);
        return 0;
    }
    if (argc > 1 && (!strcmp(argv[1], "--version") || !strcmp(argv[1], "-v"))) {
        printf("sds (SimpleDevSuite)  tree-sitter: %s\n",
#ifdef SDS_TREESITTER
               "enabled"
#else
               "not built in"
#endif
        );
        return 0;
    }
    const char *dir = argc > 1 ? argv[1] : ".";
    char rp[PATH_MAX];
    if (!realpath(dir, rp)) { fprintf(stderr, "sds: bad path: %s\n", dir); return 1; }
    struct stat st;
    if (stat(rp, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "sds: not a directory: %s\n", rp);
        return 1;
    }
    if (chdir(rp) != 0) { /* non-fatal */ }

    cfg_load();
    kw_index_build();
    hist_load();

    root = node_new("", rp, 1, NULL);
    node_load(root);
    tree_rebuild();
    git_refresh();

    initscr();
    raw();
    noecho();
    keypad(stdscr, TRUE);
    set_escdelay(25);
    {
        const char dirs[6] = { 'A', 'B', 'D', 'C', 'H', 'F' };
        const int  dmap[6] = { D_UP, D_DOWN, D_LEFT, D_RIGHT, D_HOME, D_END };
        char seq[24];
        for (int mod = 2; mod <= 8; mod++)
            for (int d = 0; d < 6; d++) {
                snprintf(seq, sizeof seq, "\033[1;%d%c", mod, dirs[d]);
                define_key(seq, MK(mod, dmap[d]));
            }
        define_key("\033[3;3~", K_ADEL);
        define_key("\033[2;3~", K_AINS);
        define_key("\033[200~", K_PSTART);
        define_key("\033[201~", K_PEND);
    }
    printf("\033[?2004h");                       /* bracketed paste on */
    fflush(stdout);

    if (has_colors()) {
        start_color();
        use_default_colors();
        apply_theme();
    }
    if (tree_autohide > 0 && COLS < tree_autohide) tree_hidden = 1;
    if (cfg_warn[0]) set_msg("%s", cfg_warn);

    int need_draw = 1;
    for (;;) {
        /* With a live shell the loop must not block in getch(), or its output
         * would only appear when a key is pressed. Poll instead, and redraw
         * only when something actually changed. */
        int live = any_live_term();
        g_timeout = live ? 20 : -1;
        timeout(g_timeout);

        if (need_draw) { draw(); need_draw = 0; }
        int c = read_key_raw();
        if (c == ERR) {                       /* poll tick, no key */
            if (pump_all_terms()) need_draw = 1;
            continue;
        }
        if (pump_all_terms()) need_draw = 1;
        need_draw = 1;
        if (c == K_NONE || c == KEY_RESIZE) continue;
        if (show_help) { show_help = 0; continue; }

        /* a focused terminal swallows everything except the app-level keys */
        if (cur >= 0 && tabs[cur]->kind == TAB_TERM) {
            Term *t = tabs[cur]->term;
            if (c == MK(2, D_UP) || c == KEY_SPREVIOUS) {      /* Shift+PgUp */
                if (t) t->sb_view = min2(t->sb_n, t->sb_view + t->rows / 2);
                continue;
            }
            if (c == MK(2, D_DOWN) || c == KEY_SNEXT) {
                if (t) t->sb_view = max2(0, t->sb_view - t->rows / 2);
                continue;
            }
            int app = (c == kb[KB_QUIT] || c == kb[KB_CLOSE_TAB] ||
                       c == kb[KB_HELP] || c == kb[KB_TERM] ||
                       c == kb[KB_TAB_PREV] || c == kb[KB_TAB_NEXT] ||
                       c == kb[KB_SIDEBAR] || c == kb[KB_QUICKOPEN] ||
                       c == kb[KB_TREE_UP] || c == kb[KB_TREE_DOWN] ||
                       c == kb[KB_TREE_OPEN] || c == kb[KB_TREE_COLLAPSE] ||
                       c == kb[KB_TREE_EXPAND] ||
                       c == kb[KB_PANE_LEFT] || c == kb[KB_PANE_RIGHT] ||
                       c == kb[KB_PANE_UP] || c == kb[KB_PANE_DOWN] ||
                       c == kb[KB_PANE_CLOSE] || shift_digit(c) >= 0 ||
                       (c >= ALT('1') && c <= ALT('9')));
            if (!app) {
                if (t && !t->dead) term_key(t, c);
                else if (t && t->dead && c != K_NONE)
                    set_msg("shell exited — Alt+W closes this tab", NULL);
                continue;
            }
        }

        if (c != kb[KB_CLOSE_TAB]) pending_close = 0;
        if (c != kb[KB_QUIT])      pending_quit = 0;
        msg[0] = 0;

        /* ── app-level ── */
        /* An if-chain rather than a switch: the bindings come from the config
         * at runtime, so they are not case-label constants. The second key on
         * some lines is a fixed alias that has always worked. */
        if (c == kb[KB_TREE_UP])         { if (tsel > 0) tsel--;        continue; }
        if (c == kb[KB_TREE_DOWN])       { if (tsel < nvis - 1) tsel++; continue; }
        if (c == kb[KB_TREE_COLLAPSE])   { tree_collapse();             continue; }
        if (c == kb[KB_TREE_EXPAND])     { tree_expand();               continue; }
        if (c == kb[KB_TREE_OPEN])       { tree_open_selected();        continue; }
        if (c == kb[KB_DEL_ENTRY])       { tree_delete_selected();      continue; }
        if (c == kb[KB_NEW_ENTRY])       { tree_new_entry();            continue; }
        if (c == kb[KB_REFRESH] || c == ALT('e')) {
            tree_refresh(); set_msg("tree refreshed", NULL);            continue;
        }
        if (c == kb[KB_TAB_PREV] || c == ALT('[')) {
            if (ntabs) set_cur((cur + ntabs - 1) % ntabs);
            continue;
        }
        if (c == kb[KB_TAB_NEXT] || c == ALT(']')) {
            if (ntabs) set_cur((cur + 1) % ntabs);
            continue;
        }
        if (c == kb[KB_PANE_LEFT])  { pane_focus_dir(D_LEFT);  continue; }
        if (c == kb[KB_PANE_RIGHT]) { pane_focus_dir(D_RIGHT); continue; }
        if (c == kb[KB_PANE_UP])    { pane_focus_dir(D_UP);    continue; }
        if (c == kb[KB_PANE_DOWN])  { pane_focus_dir(D_DOWN);  continue; }
        if (c == kb[KB_PANE_CLOSE]) { pane_close();            continue; }
        {
            int d = shift_digit(c);              /* Alt+Shift+1..9 → pane N */
            if (d >= 1 && d <= 9) {
                if (d - 1 < ntabs) pane_show_tab(d - 1);
                else set_msg("that tab is not open%s", "");
                continue;
            }
        }
        if (c == kb[KB_TERM])            { open_terminal();             continue; }
        if (c == kb[KB_CLOSE_TAB])       { act_close();                 continue; }
        if (c == kb[KB_SAVE] || c == ALT('s')) { act_save();            continue; }
        if (c == kb[KB_HELP])            { show_help = 1;               continue; }
        if (c == kb[KB_RUN])             { run_command();               continue; }
        if (c == kb[KB_QUIT])            { if (act_quit()) goto done;   continue; }
        if (c == kb[KB_QUICKOPEN])       { do_quickopen();              continue; }
        if (c == kb[KB_SIDEBAR])         { tree_toggle();               continue; }
        if (c == kb[KB_WRAP]) {
            wrap = !wrap;
            for (int i = 0; i < ntabs; i++) tabs[i]->subrow = 0;
            set_msg(wrap ? "word wrap on" : "word wrap off", NULL);
            continue;
        }
        if (c >= ALT('1') && c <= ALT('9')) {
            focus_tab(c - ALT('1'));
            continue;
        }
        if (c == K_PSTART) { handle_bracketed_paste(); continue; }

        /* ── editor ── */
        if (cur < 0) {
            if (c == kb[KB_FIND] || c == kb[KB_GOTO])
                set_msg("open a file first", NULL);
            continue;
        }
        /* ── PDF: a read-only viewer, so only movement and copying apply ── */
        if (tabs[cur]->kind == TAB_PDF) {
            Buf *b = tabs[cur];
            Pdf *pf = b->pdf;
            if (c == KEY_RIGHT || c == ' ' || c == 'n') {
                if (pf->page + 1 < pf->npg) pdf_page_into(b, pf->page + 1);
                else set_msg("last page%s", "");
                continue;
            }
            if (c == KEY_LEFT || c == 'p') {
                if (pf->page > 0) pdf_page_into(b, pf->page - 1);
                else set_msg("first page%s", "");
                continue;
            }
            if (c == kb[KB_GOTO]) {                       /* Ctrl+G: page number */
                char in[32] = "";
                if (prompt("Page: ", in, sizeof in, NULL, 0) && in[0]) {
                    int n = atoi(in);
                    if (n >= 1 && n <= pf->npg) pdf_page_into(b, n - 1);
                    else set_msg("no such page%s", "");
                }
                continue;
            }
            if (c == kb[KB_FIND])      { do_find();   continue; }
            if (c == kb[KB_FIND_NEXT]) { find_next(); continue; }
            switch (c) {
                case KEY_UP:    move_cursor(b, M_UP, 0);    break;
                case KEY_DOWN:  move_cursor(b, M_DOWN, 0);  break;
                case KEY_HOME:  move_cursor(b, M_HOME, 0);  break;
                case KEY_END:   move_cursor(b, M_END, 0);   break;
                case KEY_PPAGE: move_cursor(b, M_PGUP, 0);  break;
                case KEY_NPAGE: move_cursor(b, M_PGDN, 0);  break;
                case MK(2, D_UP):    move_cursor(b, M_UP, 1);    break;
                case MK(2, D_DOWN):  move_cursor(b, M_DOWN, 1);  break;
                case MK(2, D_LEFT):  move_cursor(b, M_LEFT, 1);  break;
                case MK(2, D_RIGHT): move_cursor(b, M_RIGHT, 1); break;
                case MK(5, D_HOME):  move_cursor(b, M_DOCHOME, 0); break;
                case MK(5, D_END):   move_cursor(b, M_DOCEND, 0);  break;
                case CTRL('c'):      ed_copy(b, 0); break;
                case CTRL('a'):
                    b->sel = 1; b->ay = 0; b->ax = 0;
                    b->cy = b->n - 1; b->cx = b->ln[b->cy].len;
                    break;
                case 27: b->sel = 0; find_show = 0; break;
                default: break;
            }
            continue;
        }
        if (tabs[cur]->kind != TAB_FILE) continue;   /* terminals handled above */
        Buf *b = tabs[cur];
        /* configurable editor actions, again as an if-chain */
        if (c == kb[KB_FIND])      { do_find();     continue; }
        if (c == kb[KB_FIND_NEXT]) { find_next();   continue; }
        if (c == kb[KB_REPLACE])   { do_replace();  continue; }
        if (c == kb[KB_GOTO])      { do_goto();     continue; }
        if (c == kb[KB_COMPLETE])  { do_complete(); continue; }
        if (c == kb[KB_MOVE_UP])   { ed_move_lines(b, 0); continue; }
        if (c == kb[KB_MOVE_DOWN]) { ed_move_lines(b, 1); continue; }
        switch (c) {
            /* movement */
            case KEY_UP:        move_cursor(b, M_UP, 0);      break;
            case KEY_DOWN:      move_cursor(b, M_DOWN, 0);    break;
            case KEY_LEFT:      move_cursor(b, M_LEFT, 0);    break;
            case KEY_RIGHT:     move_cursor(b, M_RIGHT, 0);   break;
            case KEY_HOME:      move_cursor(b, M_HOME, 0);    break;
            case KEY_END:       move_cursor(b, M_END, 0);     break;
            case KEY_PPAGE:     move_cursor(b, M_PGUP, 0);    break;
            case KEY_NPAGE:     move_cursor(b, M_PGDN, 0);    break;
            case MK(2, D_UP):    move_cursor(b, M_UP, 1);     break;
            case MK(2, D_DOWN):  move_cursor(b, M_DOWN, 1);   break;
            case MK(2, D_LEFT):  move_cursor(b, M_LEFT, 1);   break;
            case MK(2, D_RIGHT): move_cursor(b, M_RIGHT, 1);  break;
            case MK(2, D_HOME):  move_cursor(b, M_HOME, 1);   break;
            case MK(2, D_END):   move_cursor(b, M_END, 1);    break;
            case MK(5, D_LEFT):  move_cursor(b, M_WORDL, 0);  break;
            case MK(5, D_RIGHT): move_cursor(b, M_WORDR, 0);  break;
            case MK(6, D_LEFT):  move_cursor(b, M_WORDL, 1);  break;
            case MK(6, D_RIGHT): move_cursor(b, M_WORDR, 1);  break;
            case MK(5, D_HOME):  move_cursor(b, M_DOCHOME, 0); break;
            case MK(5, D_END):   move_cursor(b, M_DOCEND, 0);  break;
            case MK(6, D_HOME):  move_cursor(b, M_DOCHOME, 1); break;
            case MK(6, D_END):   move_cursor(b, M_DOCEND, 1);  break;
            case MK(5, D_UP):                       /* scroll viewport */
                if (wrap) {
                    if (b->subrow > 0) b->subrow--;
                    else if (b->rowoff > 0) {
                        b->rowoff--;
                        b->subrow = line_rows(b, b->rowoff, g_wtw) - 1;
                    }
                } else if (b->rowoff > 0) b->rowoff--;
                if (b->cy >= b->rowoff + LINES - 2) b->cy--;
                break;
            case MK(5, D_DOWN):
                if (wrap) {
                    if (++b->subrow >= line_rows(b, b->rowoff, g_wtw)) {
                        b->subrow = 0;
                        if (b->rowoff < b->n - 1) b->rowoff++;
                    }
                } else if (b->rowoff < b->n - 1) b->rowoff++;
                if (b->cy < b->rowoff) b->cy++;
                break;
            /* editing */
            case '\r': case '\n': case KEY_ENTER: ed_enter(b);     break;
            case KEY_BACKSPACE: case 127: case 8: ed_backspace(b); break;
            case KEY_DC:                          ed_delete(b);    break;
            case '\t':                            ed_tab(b, 0);    break;
            case KEY_BTAB:                        ed_tab(b, 1);    break;
            case CTRL('z'):                       do_undo(b);      break;
            case CTRL('y'):                       do_redo(b);      break;
            case CTRL('c'):                       ed_copy(b, 0);   break;
            case CTRL('x'):                       ed_copy(b, 1);   break;
            case CTRL('v'):
                if (clip) ed_paste_text(b, clip, cliplen);
                else set_msg("clipboard empty (use Ctrl+Shift+V for terminal paste)", NULL);
                break;
            case CTRL('a'):
                b->sel = 1; b->ay = 0; b->ax = 0;
                b->cy = b->n - 1; b->cx = b->ln[b->cy].len;
                break;
            case CTRL('d'):                       ed_dup_line(b);  break;
            case CTRL('k'):                       ed_del_line(b);  break;
            case 31: /* Ctrl+/ */                 ed_toggle_comment(b); break;
            case ALT('o'):                        ed_open_below(b); break;
            case 27:
                b->sel = 0; find_show = 0;
                break;
            default:
                if (c >= 32 && c != 127 && c < 256) ed_type(b, c);
        }
    }
done:
    tc_restore();               /* leave the terminal's palette as we found it */
    refresh();
    printf("\033[?2004l");
    fflush(stdout);
    endwin();
    return 0;
}
