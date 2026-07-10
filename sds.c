/* sds.c — SimpleDevSuite v2
 *
 * A small terminal code editor inspired by VS Code:
 *
 *   - file tree (left), tab bar (top), editor with line numbers, status bar
 *   - syntax highlighting: C, C++, Python, Bash, Rust, SQL, JS/TS, Go,
 *     Java, Lua, Ruby, PHP, JSON, TOML/YAML/INI, Makefile
 *   - undo/redo, selections, clipboard (with OSC 52 system-clipboard copy)
 *   - incremental find, replace, go-to-line
 *   - fuzzy quick-open (Ctrl+P), word-based autocomplete (Ctrl+Space)
 *   - auto-indent, bracket auto-close/skip/match-highlight
 *   - line ops: move, duplicate, delete, toggle comment, block (de)indent
 *   - bracketed paste, run-a-shell-command
 *
 * The mod key is Alt for app-level things; editing chords follow VS Code
 * where the terminal allows (see Alt+H in the app for the full list).
 *
 * Build:   cc -O2 -Wall -o sds sds.c -lncursesw
 * Run:     ./sds [directory]
 *
 * Known simplifications: editing is byte-based (UTF-8 loads/saves fine,
 * cursor steps per byte); no multi-cursor; no LSP.
 */

#define _XOPEN_SOURCE 700
#include <ctype.h>
#include <dirent.h>
#include <limits.h>
#include <locale.h>
#include <ncurses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define TABSTOP    4
#define TREE_W     30
#define MAX_TABS   32
#define UNDO_MAX   2000
#define QO_MAX     10000   /* quick-open file cap */

/* ── key codes ────────────────────────────────────────────────────── */
/* Modified arrows/home/end arrive as CSI "1;<mod><dir>"; we register
 * every combination with define_key() so ncurses hands back one code.
 * mod: 2=Shift 3=Alt 4=Alt+Shift 5=Ctrl 6=Ctrl+Shift.
 * dir: 0=Up 1=Down 2=Left 3=Right 4=Home 5=End.                       */
#define MK(mod, dir) (2000 + (mod) * 10 + (dir))
enum { D_UP, D_DOWN, D_LEFT, D_RIGHT, D_HOME, D_END };
enum { K_PSTART = 2900, K_PEND, K_ADEL, K_AINS, K_NONE };
#define ALT(c)  (3000 + (c))
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
    " register restrict ",
    " void char short int long float double signed unsigned bool _Bool"
    " size_t ssize_t int8_t int16_t int32_t int64_t uint8_t uint16_t"
    " uint32_t uint64_t FILE NULL true false ",
    "//", "", "/*", "*/", "", "", 0, 1, 0, 1, 0 },
  { "c++", " cpp cc cxx hpp hh hxx ",
    " if else for while do switch case default return goto break continue"
    " sizeof typedef struct union enum const static extern inline volatile"
    " class namespace template typename public private protected virtual"
    " override final new delete this try catch throw using constexpr"
    " operator friend explicit mutable noexcept static_cast dynamic_cast"
    " reinterpret_cast const_cast decltype ",
    " void char short int long float double signed unsigned bool auto"
    " size_t std string vector map set pair nullptr true false NULL ",
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
#define LANG_TEXT (&langs[sizeof langs / sizeof *langs - 1])

static const Lang *lang_for(const char *path) {
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    if (!strcasecmp(base, "Makefile") || !strcasecmp(base, "GNUmakefile"))
        return &langs[16];
    const char *dot = strrchr(base, '.');
    if (!dot || !dot[1]) return LANG_TEXT;
    char pat[32];
    snprintf(pat, sizeof pat, " %s ", dot + 1);
    for (char *p = pat; *p; p++) *p = (char)tolower((unsigned char)*p);
    for (size_t i = 0; i + 1 < sizeof langs / sizeof *langs; i++)
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

typedef struct {
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
static void die(const char *m) { endwin(); fprintf(stderr, "%s\n", m); exit(1); }
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
static void urec_free(URec *r) { free(r->t); }
static void buf_free(Buf *b) {
    for (int i = 0; i < b->n; i++) free(b->ln[i].s);
    for (int i = 0; i < b->nundo; i++) urec_free(&b->undo[i]);
    for (int i = 0; i < b->nredo; i++) urec_free(&b->redo[i]);
    free(b->undo); free(b->redo);
    free(b->ln);
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
static void hl_invalidate(Buf *b, int y) { if (y < b->hl_upto) b->hl_upto = y; }

static void ins_text(Buf *b, int y, int x, const char *t, int len,
                     int *ey, int *ex) {
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

/* ── tabs ─────────────────────────────────────────────────────────── */
static void open_file(const char *path) {
    for (int i = 0; i < ntabs; i++)
        if (strcmp(tabs[i]->path, path) == 0) { cur = i; return; }
    if (ntabs == MAX_TABS) { set_msg("too many open tabs", NULL); return; }
    Buf *b = buf_load(path);
    if (!b) { set_msg("can't open %s", path); return; }
    tabs[ntabs] = b;
    cur = ntabs++;
}
static void close_tab(int i) {
    buf_free(tabs[i]);
    memmove(tabs + i, tabs + i + 1, (size_t)(ntabs - i - 1) * sizeof(Buf *));
    ntabs--;
    if (cur >= ntabs) cur = ntabs - 1;
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
static void tree_collapse(void) {
    if (nvis == 0) return;
    Node *n = vis[tsel];
    if (n->is_dir && n->expanded) { n->expanded = 0; tree_rebuild(); }
    else if (n->parent && n->parent != root)
        for (int i = 0; i < nvis; i++)
            if (vis[i] == n->parent) { tsel = i; break; }
}
/* rescan the whole tree, keeping the cursor on the same path if it survived */
static void tree_refresh(void) {
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
    if (nvis == 0) return;
    Node *n = vis[tsel];
    if (n->is_dir && !n->expanded) { n->expanded = 1; node_load(n); tree_rebuild(); }
}

/* ── lexer ────────────────────────────────────────────────────────── */
static int kw_class(const Lang *lg, const char *s, int len) {
    char pat[72];
    if (len > 63) return HA_DEF;
    pat[0] = ' ';
    for (int i = 0; i < len; i++)
        pat[1 + i] = lg->nocase ? (char)tolower((unsigned char)s[i]) : s[i];
    pat[1 + len] = ' '; pat[2 + len] = 0;
    if (lg->kw[0] && strstr(lg->kw, pat)) return HA_KW;
    if (lg->types[0] && strstr(lg->types, pat)) return HA_TYPE;
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

/* ── rendering ────────────────────────────────────────────────────── */
enum { CP_TAB_ACT = 1, CP_TAB, CP_SEL, CP_DIR, CP_STATUS, CP_LINENO, CP_MUTED,
       CP_KW, CP_TYPE, CP_STR, CP_COM, CP_NUM, CP_PRE, CP_FIND, CP_ERR };
enum { OV_SEL = 1, OV_FIND = 2, OV_BRK = 4 };

static int rx_of(Line *l, int cx) {
    int rx = 0;
    for (int i = 0; i < cx && i < l->len; i++)
        rx = (l->s[i] == '\t') ? rx + TABSTOP - rx % TABSTOP : rx + 1;
    return rx;
}
static int cx_of_rx(Line *l, int rx) {       /* render col → byte index */
    int cur = 0;
    for (int i = 0; i < l->len; i++) {
        cur = (l->s[i] == '\t') ? cur + TABSTOP - cur % TABSTOP : cur + 1;
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
        ech = xrealloc(ech, (size_t)cap * TABSTOP + 8);
        eat = xrealloc(eat, (size_t)cap * TABSTOP + 8);
        eov = xrealloc(eov, (size_t)cap * TABSTOP + 8);
    }
    lex_line(b->lang, l->s, l->len, l->hst, hat);
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
            while (n % TABSTOP);
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
        snprintf(t, sizeof t, " %s%s ", tabs[i]->name, tabs[i]->dirty ? "*" : "");
        int pair = (i == cur) ? CP_TAB_ACT : CP_TAB;
        attron(COLOR_PAIR(pair));
        if (i == cur) attron(A_BOLD);
        mvaddnstr(0, x, t, w - x);
        if (i == cur) attroff(A_BOLD);
        attroff(COLOR_PAIR(pair));
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
    int rows = h - 2;
    if (tsel < toff) toff = tsel;
    if (tsel >= toff + rows) toff = tsel - rows + 1;
    for (int r = 0; r < rows; r++) {
        int i = toff + r;
        move(1 + r, 0);
        clrtoeol();
        attron(COLOR_PAIR(CP_MUTED));
        mvaddch(1 + r, TREE_W, ACS_VLINE);
        attroff(COLOR_PAIR(CP_MUTED));
        if (i >= nvis) continue;
        Node *n = vis[i];
        char line[512];
        const char *mark = n->is_dir ? (n->expanded ? "v " : "> ") : "  ";
        snprintf(line, sizeof line, "%*s%s%s", n->depth * 2, "", mark, n->name);
        if (i == tsel) attron(COLOR_PAIR(CP_SEL) | A_BOLD);
        else if (n->is_dir) attron(COLOR_PAIR(CP_DIR));
        mvaddnstr(1 + r, 1, line, TREE_W - 2);
        if (i == tsel) {
            int len = (int)strlen(line);
            for (int x = 1 + len; x < TREE_W - 1; x++) mvaddch(1 + r, x, ' ');
        }
        if (i == tsel) attroff(COLOR_PAIR(CP_SEL) | A_BOLD);
        else if (n->is_dir) attroff(COLOR_PAIR(CP_DIR));
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
static void draw_editor(int h, int w) {
    int x0 = TREE_W + 1, rows = h - 2, ew = w - x0;
    if (cur < 0) {
        const char *hint[] = {
            "Alt+Up/Down   browse the file tree",
            "Alt+Enter     open file / toggle folder",
            "Ctrl+P        quick-open by fuzzy name",
            "Alt+H         all keybindings",
        };
        for (int i = 0; i < 4; i++) {
            attron(COLOR_PAIR(CP_MUTED));
            mvaddnstr(1 + rows / 2 - 2 + i, x0 + 3, hint[i], ew - 3);
            attroff(COLOR_PAIR(CP_MUTED));
        }
        return;
    }
    Buf *b = tabs[cur];
    int gut = 1;
    for (int n = b->n; n; n /= 10) gut++;
    if (gut < 4) gut = 4;
    if (gut > 10) gut = 10;
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

    int r = 0;
    for (int i = b->rowoff; i < b->n && r < rows; i++) {
        int startseg = (wrap && i == b->rowoff) ? b->subrow : 0;
        if (startseg == 0) {                       /* number the line's first row */
            char num[16];
            snprintf(num, sizeof num, "%*d", gut, i + 1);
            attron(COLOR_PAIR(CP_LINENO));
            mvaddstr(1 + r, x0, num);
            attroff(COLOR_PAIR(CP_LINENO));
        }
        r += draw_row(b, 1 + r, x0 + gut + 1, i, tw, rows - r, startseg);
    }

    if (wrap) {
        int cr = -b->subrow;
        for (int i = b->rowoff; i < b->cy; i++) cr += line_rows(b, i, tw);
        int seg = min2(rx / tw, line_rows(b, b->cy, tw) - 1);  /* clamp at EOL */
        cr += seg;
        cr = max2(0, min2(cr, rows - 1));
        move(1 + cr, x0 + gut + 1 + (rx - seg * tw));
    } else {
        move(1 + (b->cy - b->rowoff), x0 + gut + 1 + (rx - b->coloff));
    }
}
static void draw_status(int h, int w) {
    attron(COLOR_PAIR(CP_STATUS));
    move(h - 1, 0);
    for (int i = 0; i < w; i++) addch(' ');
    char left[PATH_MAX + 64];
    if (cur >= 0) {
        Buf *b = tabs[cur];
        snprintf(left, sizeof left, " %s%s   %s%s   %d:%d",
                 b->path, b->dirty ? " [+]" : "", b->lang->name,
                 wrap ? "  wrap" : "", b->cy + 1, b->cx + 1);
    } else snprintf(left, sizeof left, " %s", root->path);
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
      "  TABS                                                             ",
      "  Alt+, / Alt+.  prev / next tab                                   ",
      "  Alt+1..9       go to tab N       Alt+Shift+Up/Dn   move line     ",
      "  Alt+W          close tab         Alt+O             open line belo",
      "                                   Tab / Shift+Tab   indent/dedent ",
      "  FIND & GO                        Shift+arrows      select        ",
      "  Ctrl+F  find (Enter=next)        Ctrl+Left/Right   word jump     ",
      "  F3      find next                Ctrl+Home/End     file start/end",
      "  Ctrl+R  replace (y/n/a/q)        Ctrl+Space        autocomplete  ",
      "  Ctrl+G  go to line               Alt+Z             toggle wrap   ",
      "  Ctrl+P  quick-open file          APP                             ",
      "                                   Ctrl+S / Alt+S    save          ",
      "  Esc  clear selection/highlight   Alt+R             run command   ",
      "                                   Alt+Q             quit          ",
      "                                                                   ",
      "  any key to close this                                            ",
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
    curs_set(cur >= 0 && !show_help ? 1 : 0);
    refresh();
}

/* ── confirm dialog ───────────────────────────────────────────────── */
static int read_key(void);
/* Modal yes/no. `danger` paints the box red-ish and defaults to No.
 * y / n / Enter / arrows / Tab / Esc all behave as you'd expect.        */
static int confirm(const char *title, const char *detail, int danger) {
    int yes = !danger;                       /* destructive → default No */
    for (;;) {
        draw();
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
        }
    }
}

/* ── text input dialog ────────────────────────────────────────────── */
/* Centered single-line editor. Returns 1 on Enter, 0 on Esc. */
static int input_box(const char *title, const char *hint, char *out, size_t cap) {
    size_t n = strlen(out);
    for (;;) {
        draw();
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
static int read_key(void) {
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
    nodelay(stdscr, FALSE);
    return r;
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
            if (b->cx > 0) b->cx--;
            else if (b->cy > 0) { b->cy--; b->cx = b->ln[b->cy].len; }
            break;
        case M_RIGHT:
            if (b->cx < l->len) b->cx++;
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
    /* dedent a lone '}' — eat one tab or up to TABSTOP spaces */
    if (c == '}' && line_indent_len(l) == b->cx && b->cx > 0) {
        int cut = 0;
        if (l->s[b->cx - 1] == '\t') cut = 1;
        else while (cut < TABSTOP && cut < b->cx && l->s[b->cx - 1 - cut] == ' ')
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
        edit_del(b, b->cy, b->cx - 1, b->cy, b->cx);
    } else if (b->cy > 0) {
        edit_del(b, b->cy - 1, b->ln[b->cy - 1].len, b->cy, 0);
    }
}
static void ed_delete(Buf *b) {
    if (b->sel) { begin_action(AK_OTHER); sel_delete(b); return; }
    begin_action(AK_OTHER);
    Line *l = &b->ln[b->cy];
    if (b->cx < l->len) edit_del(b, b->cy, b->cx, b->cy, b->cx + 1);
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
        int k = TABSTOP - col % TABSTOP;
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
    for (;;) {
        draw();
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
        if (live) live(out);
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
        draw();
        int bw = min2(COLS - 4, 64), lh = min2(12, max2(nm, 1));
        int x0 = (COLS - bw) / 2, y0 = 2;
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
    for (;;) {
        draw();
        int show = min2(nw, 8);
        int py = 2 + (b->cy - b->rowoff);
        int px = TREE_W + 2;
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
    printf("\033[?2004h");
    fflush(stdout);
    getch();                          /* any key, not just Enter */

    tree_refresh();                   /* pick up a.out, build/, generated files */
    refresh();
}

/* ── app actions ──────────────────────────────────────────────────── */
static void act_save(void) {
    if (cur < 0) return;
    if (buf_save(tabs[cur]) == 0) set_msg("saved %s", tabs[cur]->name);
    else                          set_msg("save failed: %s", tabs[cur]->path);
}
static void act_close(void) {
    if (cur < 0) return;
    if (tabs[cur]->dirty && !pending_close) {
        pending_close = 1;
        set_msg("unsaved changes — Alt+W again to discard", NULL);
        return;
    }
    close_tab(cur);
    pending_close = 0;
}
static int act_quit(void) {
    int dirty = 0;
    for (int i = 0; i < ntabs; i++) dirty |= tabs[i]->dirty;
    if (dirty && !pending_quit) {
        pending_quit = 1;
        set_msg("unsaved changes — Alt+Q again to quit anyway", NULL);
        return 0;
    }
    return 1;
}

/* ── main ─────────────────────────────────────────────────────────── */
int main(int argc, char **argv) {
    setlocale(LC_ALL, "");
    const char *dir = argc > 1 ? argv[1] : ".";
    char rp[PATH_MAX];
    if (!realpath(dir, rp)) { fprintf(stderr, "sds: bad path: %s\n", dir); return 1; }
    struct stat st;
    if (stat(rp, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "sds: not a directory: %s\n", rp);
        return 1;
    }
    if (chdir(rp) != 0) { /* non-fatal */ }

    hist_load();

    root = node_new("", rp, 1, NULL);
    node_load(root);
    tree_rebuild();

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
        init_pair(CP_TAB_ACT, COLOR_BLACK,   COLOR_YELLOW);
        init_pair(CP_TAB,     COLOR_WHITE,   COLOR_BLACK);
        init_pair(CP_SEL,     COLOR_BLACK,   COLOR_YELLOW);
        init_pair(CP_DIR,     COLOR_CYAN,    -1);
        init_pair(CP_STATUS,  COLOR_BLACK,   COLOR_WHITE);
        init_pair(CP_LINENO,  COLOR_YELLOW,  -1);
        init_pair(CP_MUTED,   COLOR_BLUE,    -1);
        init_pair(CP_KW,      COLOR_MAGENTA, -1);
        init_pair(CP_TYPE,    COLOR_CYAN,    -1);
        init_pair(CP_STR,     COLOR_GREEN,   -1);
        init_pair(CP_COM,     COLOR_BLUE,    -1);
        init_pair(CP_NUM,     COLOR_RED,     -1);
        init_pair(CP_PRE,     COLOR_CYAN,    -1);
        init_pair(CP_FIND,    COLOR_BLACK,   COLOR_GREEN);
        init_pair(CP_ERR,     COLOR_WHITE,   COLOR_RED);
    }

    for (;;) {
        draw();
        int c = read_key();
        if (c == K_NONE || c == ERR || c == KEY_RESIZE) continue;
        if (show_help) { show_help = 0; continue; }
        if (c != ALT('w')) pending_close = 0;
        if (c != ALT('q')) pending_quit = 0;
        msg[0] = 0;

        /* ── app-level (Alt & friends) ── */
        switch (c) {
            case MK(3, D_UP):    if (tsel > 0) tsel--;        continue;
            case MK(3, D_DOWN):  if (tsel < nvis - 1) tsel++; continue;
            case MK(3, D_LEFT):  tree_collapse();             continue;
            case MK(3, D_RIGHT): tree_expand();               continue;
            case ALT('\n'):      tree_open_selected();        continue;
            case K_ADEL:         tree_delete_selected();      continue;
            case K_AINS:         tree_new_entry();            continue;
            case KEY_F(5): case ALT('e'):
                tree_refresh(); set_msg("tree refreshed", NULL); continue;
            case ALT(','): case ALT('['):
                if (ntabs) { cur = (cur + ntabs - 1) % ntabs; } continue;
            case ALT('.'): case ALT(']'):
                if (ntabs) { cur = (cur + 1) % ntabs; }         continue;
            case ALT('w'):  act_close();                      continue;
            case ALT('s'):  act_save();                       continue;
            case ALT('h'):  show_help = 1;                    continue;
            case ALT('r'):  run_command();                    continue;
            case ALT('q'):  if (act_quit()) goto done;        continue;
            case CTRL('s'): act_save();                       continue;
            case CTRL('p'): do_quickopen();                   continue;
            case ALT('z'):
                wrap = !wrap;
                for (int i = 0; i < ntabs; i++) tabs[i]->subrow = 0;
                set_msg(wrap ? "word wrap on" : "word wrap off", NULL);
                continue;
        }
        if (c >= ALT('1') && c <= ALT('9')) {
            int i = c - ALT('1');
            if (i < ntabs) cur = i;
            continue;
        }
        if (c == K_PSTART) { handle_bracketed_paste(); continue; }

        /* ── editor ── */
        if (cur < 0) {
            if (c == CTRL('f') || c == CTRL('g')) set_msg("open a file first", NULL);
            continue;
        }
        Buf *b = tabs[cur];
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
            /* line ops */
            case MK(4, D_UP):    ed_move_lines(b, 0);         break;
            case MK(4, D_DOWN):  ed_move_lines(b, 1);         break;
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
            /* find & go */
            case CTRL('f'):                       do_find();       break;
            case KEY_F(3):                        find_next();     break;
            case CTRL('r'):                       do_replace();    break;
            case CTRL('g'):                       do_goto();       break;
            case 0: /* Ctrl+Space */              do_complete();   break;
            case 27:
                b->sel = 0; find_show = 0;
                break;
            default:
                if (c >= 32 && c != 127 && c < 256) ed_type(b, c);
        }
    }
done:
    printf("\033[?2004l");
    fflush(stdout);
    endwin();
    return 0;
}
