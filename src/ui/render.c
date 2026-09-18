#include "core/sds.h"
#include "ui/ui.h"
#include "ui/ui_internal.h"
#include "syntax/syntax.h"
#include "editor/editor.h"
#include "markdown/markdown.h"
#include "tree/tree.h"
#include "git/git.h"
#include "terminal/terminal.h"
#include "commands/commands.h"
#include "lsp/lsp.h"

/* bracket-match highlight, recomputed each frame */
static int brk_y1 = -1, brk_x1, brk_y2, brk_x2;

/* ── rendering ────────────────────────────────────────────────────── */

int rx_of(Line *l, int cx) {
    int rx = 0;
    for (int i = 0; i < cx && i < l->len; i++)
        rx = (l->s[i] == '\t') ? rx + tabstop - rx % tabstop : rx + 1;
    return rx;
}
int cx_of_rx(Line *l, int rx) {       /* render col → byte index */
    int cur = 0;
    for (int i = 0; i < l->len; i++) {
        cur = (l->s[i] == '\t') ? cur + tabstop - cur % tabstop : cur + 1;
        if (cur > rx) return i;
    }
    return l->len;
}
/* screen rows a buffer line occupies (1 when not wrapping) */
int line_rows(Buf *b, int li, int tw) {
    if (!wrap || tw < 1) return 1;
    int n = rx_of(&b->ln[li], b->ln[li].len);
    return n < 1 ? 1 : (n + tw - 1) / tw;
}
/* Diagnostic rows under line li: one per message, capped, the last of which
 * says how many more there are. */
#define DIAG_ROWS_MAX 4
int diag_rows(Buf *b, int li) {
    if (!b->lsp) return 0;
    int n = lsp_line_diags(b, li, NULL);
    return n > DIAG_ROWS_MAX ? DIAG_ROWS_MAX : n;
}
/* Screen rows line li takes: its text, then the gap its diagnostics open. */
int line_vrows(Buf *b, int li, int tw) {
    return line_rows(b, li, tw) + diag_rows(b, li);
}
/* The horizontal scroll line li is drawn with. With line_scroll only the
 * cursor's line moves; drawing and hit-testing both ask here so they agree. */
int line_off(Buf *b, int li) {
    return (wrap || (line_scroll && li != b->cy)) ? 0 : b->coloff;
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
/* A markdown style byte: a color role in the top three bits, terminal
 * attributes in the low five. */
attr_t md_attr(unsigned char st) {
    static const short pair[8] = {
        0, CP_HEAD, CP_STR, CP_TYPE, CP_MUTED, CP_MUTED, CP_KW, CP_MUTED,
    };
    int role = st >> 5, f = st & 31;
    attr_t a = pair[role] ? COLOR_PAIR(pair[role]) : A_NORMAL;
    if (f & MS_BOLD)  a |= A_BOLD;
    if (f & MS_UNDER) a |= A_UNDERLINE;
    if (f & MS_DIM)   a |= A_DIM;
#ifdef A_ITALIC
    if (f & MS_ITAL)  a |= A_ITALIC;
#else
    if (f & MS_ITAL)  a |= A_UNDERLINE;
#endif
    return a;
}
/* case-insensitive memmem */
int ci_find(const char *hay, int hlen, const char *nee, int nlen, int from) {
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
int draw_row(Buf *b, int scr_y, int scr_x, int li, int tw, int maxrows,
                    int startseg) {
    static unsigned char *hat = NULL, *ov = NULL;
    static char *ech = NULL; static unsigned char *eat = NULL, *eov = NULL;
    static int cap = 0;
    Line *l = &b->ln[li];
    /* Everything below works on the bytes that can actually appear in the
     * pane. A line wider than the window used to be lexed, tab-expanded and
     * overlaid in full for every row it was drawn on. */
    int off = line_off(b, li);
    int vis_cols = wrap ? (startseg + maxrows) * tw : off + tw;
    int limit = 0;
    for (int col = 0; limit < l->len && col < vis_cols; limit++)
        col += (l->s[limit] == '\t') ? tabstop - col % tabstop : 1;
    int need = limit + 8;
    if (need > cap) {
        cap = need * 2;
        hat = xrealloc(hat, (size_t)cap);
        ov  = xrealloc(ov,  (size_t)cap);
        ech = xrealloc(ech, (size_t)cap * TABSTOP_MAX + 8);
        eat = xrealloc(eat, (size_t)cap * TABSTOP_MAX + 8);
        eov = xrealloc(eov, (size_t)cap * TABSTOP_MAX + 8);
    }
    hl_line(b, li, hat, limit);
    memset(ov, 0, (size_t)(limit ? limit : 1));

    int y1, x1, y2, x2;                                  /* selection */
    if (sel_norm(b, &y1, &x1, &y2, &x2) && li >= y1 && li <= y2) {
        int a = (li == y1) ? x1 : 0;
        int z = (li == y2) ? x2 : l->len;
        for (int i = a; i < z && i < limit; i++) ov[i] |= OV_SEL;
    }
    if (find_show && findq[0]) {                          /* find matches */
        int q = (int)strlen(findq), at = 0;
        while ((at = ci_find(l->s, min2(limit + q, l->len), findq, q, at)) >= 0) {
            for (int i = at; i < at + q && i < limit; i++) ov[i] |= OV_FIND;
            at += q;
        }
    }
    if (brk_y1 == li && brk_x1 < limit) ov[brk_x1] |= OV_BRK;
    if (brk_y2 == li && brk_x2 < limit) ov[brk_x2] |= OV_BRK;

    /* expand tabs, carrying attrs/overlays along */
    int n = 0;
    for (int i = 0; i < limit; i++) {
        if (l->s[i] == '\t') {
            do { ech[n] = ' '; eat[n] = hat[i]; eov[n] = ov[i]; n++; }
            while (n % tabstop);
        } else { ech[n] = l->s[i]; eat[n] = hat[i]; eov[n] = ov[i]; n++; }
    }
    if (!wrap) {
        if (n > off) {
            int from = off, to = min2(n, off + tw);
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
/* The gap under line li: each diagnostic on a row of its own, its "└" under
 * the column it points at when the message fits there, pulled left when it
 * would run off the edge. Returns the rows used. */
int draw_diags(Buf *b, int li, int scr_y, int scr_x, int tw, int maxrows) {
    const Diag *d;
    int n = lsp_line_diags(b, li, &d);
    int show = min2(n, DIAG_ROWS_MAX), used = 0;
    if (tw < 4) return min2(show, maxrows);
    for (int k = 0; k < show && used < maxrows; k++, used++) {
        static const char *label[] = { "", "error: ", "warning: ", "info: ", "hint: " };
        static const short pair[] = { 0, CP_DIAG_ERR, CP_DIAG_WARN, CP_DIAG_INFO, CP_DIAG_INFO };
        char more[48];
        const char *text = d[k].msg, *lab = label[d[k].severity];
        int sev = d[k].severity;
        if (k == DIAG_ROWS_MAX - 1 && n > DIAG_ROWS_MAX) {
            snprintf(more, sizeof more, "+%d more on this line", n - k);
            text = more; lab = ""; sev = DIAG_INFO;
        }
        int tl = (int)strcspn(text, "\n");       /* the first line says it */
        int rx = rx_of(&b->ln[li], d[k].col);
        int col = wrap ? rx % tw : rx - line_off(b, li);
        int want = 2 + (int)strlen(lab) + md_cols(text, tl);
        if (col + want > tw) col = tw - want;
        col = max2(0, min2(col, tw - 3));
        int room = tw - col - 2, ll = min2((int)strlen(lab), room);
        attrset(COLOR_PAIR(pair[sev]));
        mvaddstr(scr_y + used, scr_x + col, "└ ");
        attron(A_BOLD);
        addnstr(lab, ll);
        attroff(A_BOLD);
        addnstr(text, md_byte_at(text, tl, room - ll));
    }
    attrset(A_NORMAL);
    return used;
}

/* Where each tab ended up on the bar, so a click can be matched against the
 * same boxes that were drawn rather than against a second guess at the
 * layout. x0 < 0 means the tab scrolled off the bar. */
TabBox tab_box[MAX_TABS];

void draw_tabbar(int w) {
    for (int i = 0; i < MAX_TABS; i++) tab_box[i].x0 = tab_box[i].x1 = -1;
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
        tab_box[i].x0 = x;
        tab_box[i].x1 = min2(x + (int)strlen(t), w);
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
void draw_tree(int h) {
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
void find_bracket(Buf *b) {
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
#define CP_TERM_BASE 20          /* above every fixed CP_* pair in ui.h */
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
void draw_term(Buf *b, int ytop, int x0, int rows, int cols) {
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
