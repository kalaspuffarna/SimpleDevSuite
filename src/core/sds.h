/* sds.h — shared types, globals and helpers */
#ifndef SDS_CORE_H
#define SDS_CORE_H

#define _XOPEN_SOURCE 700
/* glibc hides struct dirent's d_type behind this. Without it every directory
 * entry needs a stat() to find out whether it is a directory, which is four
 * times the cost of a walk in a large tree. */
#define _DEFAULT_SOURCE
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
#include <time.h>
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
    int md;                    /* markdown: own lexer, own viewer */
} Lang;

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
typedef struct Md   Md;
typedef struct LspDoc LspDoc;
enum { TAB_FILE, TAB_TERM, TAB_PDF };

typedef struct {
    int   kind;                /* TAB_FILE, TAB_TERM or TAB_PDF */
    Term *term;                /* set when kind == TAB_TERM */
    Pdf  *pdf;                 /* set when kind == TAB_PDF  */
    int   pdf_img;             /* PDF tab showing the rendered page  */
    Md   *md;                  /* rendered markdown, built on demand */
    int   md_view;             /* showing that render instead of the text */
    LspDoc *lsp;               /* language server state, when one serves it */
    int   md_goto;             /* line to scroll the render to, -1 = none */
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
    int   hl_dirty;            /* last line an edit touched, for ensure_hl */
    int   hl_seen;             /* highest line a lex pass has ever reached */
    URec *undo; int nundo, undocap;
    URec *redo; int nredo, redocap;
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
    int   nkid, kidcap;
    struct Node *parent;
    int   depth;
} Node;

/* Split view. Each pane shows one tab; `cur` is always the focused pane's tab,
 * so every existing code path that reads `cur` keeps working unchanged and
 * only the few places that *set* it have to go through set_cur().           */
#define MAX_PANES 4

/* globals.c */
extern int tabstop;
extern char msg[PATH_MAX + 64];
extern int pending_close, pending_quit, show_help;
extern int wrap;
extern int line_scroll;
extern int cfg_ts_max_kb;      /* tree-sitter gives up above this file size */
extern int g_wtw;

/* util.c */
void die(const char *m);
void *xrealloc(void *p, size_t n);
void *xmalloc(size_t n);
char *xstrdup(const char *s);
void set_msg(const char *fmt, const char *a);
int word_ch(int c);
int find_exec(const char *name, char *out, size_t cap);
int dirent_is_dir(const struct dirent *e, const char *path);
int min2(int a, int b);
int max2(int a, int b);

#endif /* SDS_CORE_H */
