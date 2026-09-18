/* ui.h — tabs, panes, drawing, themes and dialogs */
#ifndef SDS_UI_H
#define SDS_UI_H

#include "core/sds.h"

/* tabs.c */
extern Buf *tabs[MAX_TABS];
extern int ntabs, cur;
void set_cur(int i);
void focus_tab(int i);
void open_file(const char *path);
void close_tab(int i);
void pane_close(void);
void pane_show_tab(int t);

/* theme.c */
/* ── color pairs ──────────────────────────────────────────────────── */
enum { CP_TAB_ACT = 1, CP_TAB, CP_SEL, CP_DIR, CP_STATUS, CP_LINENO, CP_MUTED,
       CP_KW, CP_TYPE, CP_STR, CP_COM, CP_NUM, CP_PRE, CP_FIND, CP_ERR,
       CP_HEAD, CP_DIAG_ERR, CP_DIAG_WARN, CP_DIAG_INFO };   /* < CP_TERM_BASE */

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

extern const Theme theme_tux;
extern const Theme theme_presets[];
extern const int ntheme_presets;
extern Theme theme;
int rgb_to_256(int rgb);
extern int tc_want;
void tc_restore(void);
void apply_theme(void);

/* render.c */
int rx_of(Line *l, int cx);
int cx_of_rx(Line *l, int rx);
int line_rows(Buf *b, int li, int tw);
int diag_rows(Buf *b, int li);
int line_vrows(Buf *b, int li, int tw);
int line_off(Buf *b, int li);
int ci_find(const char *hay, int hlen, const char *nee, int nlen, int from);
typedef struct { int x0, x1; } TabBox;
extern TabBox tab_box[MAX_TABS];

/* panes.c */
extern int panes[MAX_PANES];
extern int npanes, curpane;

/* ── panes ────────────────────────────────────────────────────────── */
/* 1 pane fills the area; 2 split left/right; 3 keeps the left column whole
 * and stacks the right one; 4 is a 2×2. Dividers are carved out of the area
 * so the panes never overlap them. */
typedef struct { int y, x, h, w; } Rect;
typedef struct { Rect r[MAX_PANES]; int n, vx, hy, hx, hw; } Layout;

#define PANE_MINW 16
void pane_focus_dir(int dir);
extern Layout g_lay;
extern int g_lay_hdr;
Rect pane_body(int i);
int pane_gutter(Buf *b);
int pane_textw(Buf *b, int panew);
int focused_pane_rows(void);
int cursor_row(Buf *b, int tw);
int cursor_tail(Buf *b, int tw);
void ed_scroll(Buf *b, int down, int n, int rows);

/* draw.c */
extern int help_off;
void help_scroll(int delta, int page);
void draw(void);

/* dialogs.c */
int confirm(const char *title, const char *detail, int danger);
int input_box(const char *title, const char *hint, char *out, size_t cap);

#endif /* SDS_UI_H */
