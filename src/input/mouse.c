#include "core/sds.h"
#include "input/input.h"
#include "input/input_internal.h"
#include "editor/editor.h"
#include "pdf/pdf.h"
#include "ui/ui.h"
#include "tree/tree.h"
#include "terminal/terminal.h"
#include "commands/commands.h"
#include "lsp/lsp.h"

/* ── mouse handling ───────────────────────────────────────────────── */
/* Everything here hit-tests against what the last frame actually drew —
 * tab_box and g_lay — rather than working the layout out a second time,
 * because a second guess is a second thing to keep in step. */
enum { DRAG_NONE, DRAG_SEL, DRAG_TREEW };
static int  drag_mode = DRAG_NONE, drag_pane = 0;
static long click_at = 0;                    /* double / triple click run */
static int  click_y = -1, click_x = -1, click_n = 0;

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
static int pane_at(int y, int x) {
    for (int i = 0; i < g_lay.n; i++) {
        Rect r = g_lay.r[i];
        if (y >= r.y && y < r.y + r.h && x >= r.x && x < r.x + r.w) return i;
    }
    return -1;
}
/* The tab drawn in laid-out pane i. When the area is too cramped to split,
 * the single rectangle shows the focused tab whatever pane it belongs to. */
static int lay_tab(int i) {
    if (g_lay.n <= 1) return cur;
    return (i >= 0 && i < npanes) ? panes[i] : -1;
}
static void lay_focus(int i) {
    if (g_lay.n > 1 && i >= 0 && i < npanes) { curpane = i; cur = panes[i]; }
}
/* A point in a pane's text area → the position in its buffer. */
static void pane_pos(Buf *b, Rect body, int y, int x, int *oy, int *ox) {
    int tw  = pane_textw(b, body.w);
    int rx  = x - (body.x + pane_gutter(b) + 1);
    int li = b->rowoff, seg = wrap ? b->subrow : 0, gap = 0;
    if (rx < 0) rx = 0;
    /* step down the rows as draw_pane laid them out: a line's wrapped
     * segments, then the diagnostic rows in the gap under it */
    for (int left = max2(0, y - body.y); left > 0 && li < b->n; left--) {
        if (!gap && seg + 1 < line_rows(b, li, tw)) seg++;
        else if (gap < diag_rows(b, li)) gap++;
        else { li++; seg = 0; gap = 0; }
    }
    if (li < b->n && gap) {              /* a message: go to what it points at */
        const Diag *d;
        if (lsp_line_diags(b, li, &d) >= gap) {
            *oy = li;
            *ox = min2(d[gap - 1].col, b->ln[li].len);
            return;
        }
    }
    rx += wrap ? seg * tw : line_off(b, li < b->n ? li : b->n - 1);
    if (li >= b->n) { li = b->n - 1; rx = INT_MAX; }
    if (li < 0) li = 0;
    int cx = cx_of_rx(&b->ln[li], rx);
    /* a click can land inside a multi-byte character; snap to its start */
    while (cx > 0 && cx < b->ln[li].len && utf8_cont((unsigned char)b->ln[li].s[cx]))
        cx--;
    *oy = li;
    *ox = cx;
}
static void select_word_at(Buf *b) {
    Line *l = &b->ln[b->cy];
    int i = b->cx;
    if (i >= l->len || !word_ch(l->s[i])) {
        if (i > 0 && word_ch(l->s[i - 1])) i--;
        else return;                          /* nothing wordy under the click */
    }
    int s = i, e = i;
    while (s > 0 && word_ch(l->s[s - 1])) s--;
    while (e < l->len && word_ch(l->s[e])) e++;
    b->sel = 1; b->ay = b->cy; b->ax = s; b->cx = e;
}
static void select_line_at(Buf *b) {
    b->sel = 1; b->ay = b->cy; b->ax = 0;
    if (b->cy < b->n - 1) { b->cy++; b->cx = 0; }
    else b->cx = b->ln[b->cy].len;
}
static void mouse_wheel(int y, int x, int down) {
    if (y == 0) {                             /* over the tab bar: walk tabs */
        if (ntabs) set_cur((cur + (down ? 1 : ntabs - 1)) % ntabs);
        return;
    }
    if (!tree_hidden && x <= tree_w) {
        tsel = max2(0, min2(nvis - 1, tsel + (down ? 3 : -3)));
        return;
    }
    int pi = pane_at(y, x);
    if (pi < 0) return;
    int ti = lay_tab(pi);
    if (ti < 0) return;
    Buf *b = tabs[ti];
    if (b->kind == TAB_TERM) {
        Term *t = b->term;
        if (t) t->sb_view = max2(0, min2(t->sb_n, t->sb_view + (down ? -3 : 3)));
    } else if (b->kind == TAB_PDF && b->pdf_img) {
        int cw, chh;
        cell_px(&cw, &chh);
        pdf_scroll(b, (down ? 3 : -3) * chh);
    } else if (b->md_view) {
        md_scroll(b, down ? 3 : -3);
    } else {
        ed_scroll(b, down, 3, pane_body(pi).h);
    }
}
void handle_mouse(void) {
    int y = mev.y, x = mev.x;

    /* A drag belongs to whatever the press started on, no matter where the
     * pointer has wandered to since. */
    if (mev.motion) {
        if (drag_mode == DRAG_TREEW) {
            int lim = min2(100, COLS - PANE_MINW - 2);
            if (lim >= 10) tree_w = max2(10, min2(x, lim));
            return;
        }
        if (drag_mode != DRAG_SEL || drag_pane >= g_lay.n) return;
        int ti = lay_tab(drag_pane);
        if (ti < 0 || tabs[ti]->kind == TAB_TERM || tabs[ti]->md_view) return;
        Buf *b = tabs[ti];
        Rect body = pane_body(drag_pane);
        if (body.h < 1) return;
        /* dragging past an edge scrolls, so a selection can run off-screen */
        if (y < body.y)                  ed_scroll(b, 0, 1, body.h);
        else if (y >= body.y + body.h)   ed_scroll(b, 1, 1, body.h);
        int cy, cx;
        pane_pos(b, body, min2(max2(y, body.y), body.y + body.h - 1), x, &cy, &cx);
        if (!b->sel) { b->sel = 1; b->ay = b->cy; b->ax = b->cx; }
        b->cy = cy; b->cx = cx;
        return;
    }
    if (!mev.press) { drag_mode = DRAG_NONE; return; }
    if (mev.btn == MB_WHEEL_UP || mev.btn == MB_WHEEL_DOWN) {
        mouse_wheel(y, x, mev.btn == MB_WHEEL_DOWN);
        return;
    }
    if (mev.btn > MB_RIGHT) return;           /* horizontal wheel, unused */

    if (y == 0) {                             /* tab bar */
        for (int i = 0; i < ntabs; i++)
            if (tab_box[i].x0 >= 0 && x >= tab_box[i].x0 && x < tab_box[i].x1) {
                if (mev.btn == MB_MID) {      /* browser habit: close it */
                    if (i != cur) pending_close = 0;
                    set_cur(i);
                    act_close();
                } else focus_tab(i);
                return;
            }
        return;
    }
    if (y >= LINES - 1) return;               /* status bar */

    if (!tree_hidden && x <= tree_w) {        /* sidebar */
        if (mev.btn == MB_RIGHT) return;
        if (x == tree_w) { drag_mode = DRAG_TREEW; return; }   /* its edge */
        int i = toff + (y - 1);
        if (i < 0 || i >= nvis) return;
        tsel = i;
        Node *n = vis[i];
        if (n->is_dir) {
            n->expanded = !n->expanded;
            if (n->expanded) node_load(n);
            tree_rebuild();
        }
        else if (mev.btn == MB_MID) tree_open_pane_selected();
        else if (mev.btn == MB_LEFT) open_file(n->path);
        return;
    }

    int pi = pane_at(y, x);
    if (pi < 0) return;
    lay_focus(pi);
    if (g_lay_hdr && y == g_lay.r[pi].y) return;   /* the header only focuses */
    int ti = lay_tab(pi);
    if (ti < 0 || mev.btn != MB_LEFT) return;
    Buf *b = tabs[ti];
    /* a terminal or a rendered page has no text cursor to place */
    if (b->kind == TAB_TERM || (b->kind == TAB_PDF && b->pdf_img) ||
        b->md_view) return;

    Rect body = pane_body(pi);
    if (body.h < 1) return;
    int cy, cx;
    pane_pos(b, body, y, x, &cy, &cx);

    long t = now_ms();
    if (t - click_at < 400 && y == click_y && x == click_x) click_n++;
    else click_n = 1;
    click_at = t; click_y = y; click_x = x;

    if (mev.shift && b->sel) { b->cy = cy; b->cx = cx; return; }   /* extend */
    b->cy = cy; b->cx = cx; b->sel = 0;
    if (click_n == 2)      select_word_at(b);
    else if (click_n >= 3) select_line_at(b);
    else { drag_mode = DRAG_SEL; drag_pane = pi; }
}
