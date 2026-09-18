#include "core/sds.h"
#include "ui/ui.h"
#include "ui/ui_internal.h"
#include "input/input.h"
#include "pdf/pdf.h"
#include "tree/tree.h"

int panes[MAX_PANES] = { -1, -1, -1, -1 };
int npanes = 1, curpane = 0;
#define PANE_MINH 4

Layout pane_layout(Rect a) {
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
Rect editor_area(void) {
    int x0 = tree_hidden ? 0 : tree_w + 1;
    Rect a = { 1, x0, LINES - 2, COLS - x0 };
    if (a.h < 1) a.h = 1;
    if (a.w < 1) a.w = 1;
    return a;
}
/* Nearest pane in a direction, by rectangle centre — distance along the axis
 * dominates, so a wide neighbour never wins over the one actually beside you. */
void pane_focus_dir(int dir) {
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
int g_cy = -1, g_cx = -1;

/* The layout the last frame was drawn with, for hit-testing clicks. n == 0
 * means nothing is open and there is nothing to click into. */
Layout g_lay;
int    g_lay_hdr;

/* A pane's text area: its rectangle less the header row the split view adds. */
Rect pane_body(int i) {
    Rect r = g_lay.r[i];
    if (g_lay_hdr) { r.y++; r.h--; }
    return r;
}
/* Columns a pane spends on line numbers, and the text width left over.
 * draw_pane works this out the same way; both have to agree or a click
 * lands on the wrong character. */
int pane_gutter(Buf *b) {
    if (b->kind == TAB_PDF) return 1;
    int gut = 1;
    for (int n = b->n; n; n /= 10) gut++;
    if (gut < 4) gut = 4;
    if (gut > 10) gut = 10;
    return gut;
}
int pane_textw(Buf *b, int panew) {
    int tw = panew - pane_gutter(b) - 1;
    if (tw < 1) tw = 1;
    /* when wrapping, leave one column free so a cursor sitting at the wrap
     * point (rx == tw) still lands on screen instead of past the edge */
    if (wrap && tw > 1) tw--;
    return tw;
}
/* Text rows the focused pane shows, for the keys that scroll by a viewport. */
int focused_pane_rows(void) {
    if (g_lay.n < 1) return LINES - 2;
    return pane_body(min2(curpane, g_lay.n - 1)).h;
}
/* The cursor's row inside its pane, counted from the top of the viewport
 * through every wrapped segment and diagnostic gap above it. */
int cursor_row(Buf *b, int tw) {
    int rx = rx_of(&b->ln[b->cy], b->cx);
    int seg = wrap ? min2(rx / tw, line_rows(b, b->cy, tw) - 1) : 0;
    int row = (wrap ? -b->subrow : 0) + seg;
    for (int i = b->rowoff; i < b->cy; i++) row += line_vrows(b, i, tw);
    return row;
}
/* Rows worth keeping on screen below the cursor: its line's diagnostics,
 * once the cursor is on the last row of that line's text. Reading the error
 * for the line being edited should not need a scroll. */
int cursor_tail(Buf *b, int tw) {
    int rx = rx_of(&b->ln[b->cy], b->cx), last = line_rows(b, b->cy, tw) - 1;
    int seg = wrap ? min2(rx / tw, last) : 0;
    return seg == last ? diag_rows(b, b->cy) : 0;
}
/* Scroll a buffer's viewport by n rows. The cursor comes along only as far
 * as it has to: draw_pane pulls the offsets back to the cursor every frame,
 * so a viewport that left it behind would snap straight back. */
void ed_scroll(Buf *b, int down, int n, int rows) {
    for (int k = 0; k < n; k++) {
        if (down) {
            if (wrap) {
                if (++b->subrow >= line_rows(b, b->rowoff, g_wtw)) {
                    b->subrow = 0;
                    if (b->rowoff < b->n - 1) b->rowoff++;
                }
            } else if (b->rowoff < b->n - 1) b->rowoff++;
            if (b->cy < b->rowoff) b->cy++;
        } else {
            if (wrap) {
                if (b->subrow > 0) b->subrow--;
                else if (b->rowoff > 0) {
                    b->rowoff--;
                    b->subrow = line_rows(b, b->rowoff, g_wtw) - 1;
                }
            } else if (b->rowoff > 0) b->rowoff--;
        }
    }
    /* Scrolling up can leave the cursor below the pane. Walk it back up,
     * counting diagnostic gaps the way draw_pane does, or the next frame
     * would scroll straight back down to it. */
    while (rows > 1 && b->cy > b->rowoff &&
           cursor_row(b, g_wtw) + cursor_tail(b, g_wtw) >= rows)
        b->cy--;
    if (b->cx > b->ln[b->cy].len) b->cx = b->ln[b->cy].len;
}
