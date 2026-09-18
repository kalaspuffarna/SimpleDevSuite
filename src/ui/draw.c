#include "core/sds.h"
#include "ui/ui.h"
#include "ui/ui_internal.h"
#include "syntax/syntax.h"
#include "pdf/pdf.h"
#include "markdown/markdown.h"
#include "tree/tree.h"
#include "git/git.h"
#include "terminal/terminal.h"

/* The rendered markdown view: styled text, no cursor, no line numbers. It
 * scrolls on its own `rowoff` so switching back to the source doesn't
 * disturb where the editor was. */
static void draw_md(Buf *b, Rect body) {
    int width = body.w - 2;                 /* a column of air on either side */
    if (width < 8) width = 8;
    Md *m = md_view_of(b, width);
    if (b->md_goto >= 0) {
        m->rowoff = md_row_for_src(m, b->md_goto);
        b->md_goto = -1;
    }
    if (m->rowoff > m->n - body.h) m->rowoff = m->n - body.h;
    if (m->rowoff < 0) m->rowoff = 0;
    for (int r = 0; r < body.h && m->rowoff + r < m->n; r++) {
        Line *l = &m->ln[m->rowoff + r];
        unsigned char *at = m->at[m->rowoff + r];
        int limit = md_byte_at(l->s, l->len, body.w - 1);
        for (int i = 0, col = 0; i < limit; ) {
            int j = i;
            while (j < limit && at[j] == at[i]) j++;
            attrset(md_attr(at[i]));
            mvaddnstr(body.y + r, body.x + 1 + col, l->s + i, j - i);
            col += md_cols(l->s + i, j - i);
            i = j;
        }
    }
    attrset(A_NORMAL);
}
static void draw_pane(Rect pr, int pi, int ti, int focused, int hdr) {
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
    if (b->md_view) { draw_md(b, pr); return; }
    /* line numbers make no sense for a PDF page, so it gets a plain margin.
     * The widths come from the shared helpers because a click has to be able
     * to work out the same ones. */
    int nums = (b->kind != TAB_PDF);
    int gut = pane_gutter(b);
    int tw  = pane_textw(b, ew);
    g_wtw = tw;
    /* a PDF page is re-flowed to whatever width the pane ended up with, so it
     * never needs sideways scrolling */
    if (b->kind == TAB_PDF && b->pdf->laid_w != tw) pdf_relayout(b, tw);
    if (b->kind == TAB_PDF && b->pdf_img) {
        gfx_request(pi, b, y0, x0, ew, rows);
        if (b->pdf_img) return;            /* cleared again if rendering failed */
    }
    int rx = rx_of(&b->ln[b->cy], b->cx);

    if (!wrap) {
        if (b->cy < b->rowoff) b->rowoff = b->cy;
        if (b->cy >= b->rowoff + rows) b->rowoff = b->cy - rows + 1;
        b->subrow = 0;
        if (line_scroll) {
            /* Pinned to the start edge: the cursor's line scrolls no further
             * than it takes to show the cursor, so a line's beginning is on
             * screen whenever the cursor is within a screen-width of it. */
            b->coloff = rx >= tw ? rx - tw + 1 : 0;
        } else {
            if (rx < b->coloff) b->coloff = rx;
            if (rx >= b->coloff + tw) b->coloff = rx - tw + 1;
        }
    } else {
        b->coloff = 0;
        int cseg = min2(rx / tw, line_rows(b, b->cy, tw) - 1);
        if (b->cy < b->rowoff) { b->rowoff = b->cy; b->subrow = 0; }
        /* cheap first guess so the loop below stays short on big jumps */
        if (b->cy - b->rowoff >= rows) { b->rowoff = max2(0, b->cy - rows + 1); b->subrow = 0; }
        if (b->rowoff == b->cy && b->subrow > cseg) b->subrow = cseg;
        if (b->subrow >= line_rows(b, b->rowoff, tw)) b->subrow = 0;
    }
    /* Scroll down a visual row at a time until the cursor is in the pane —
     * and the diagnostics under its line with it, when they fit. Rows are
     * counted with the gaps, so an error above does not push the cursor off
     * the bottom. */
    int tail = cursor_tail(b, tw);
    for (;;) {
        int row = cursor_row(b, tw);
        if (row <= 0 || row + tail < rows) break;
        if (wrap && ++b->subrow < line_rows(b, b->rowoff, tw)) continue;
        b->subrow = 0;
        b->rowoff++;
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
        if (b->lsp && vr < rows)                   /* the gap for its errors */
            vr += draw_diags(b, i, y0 + vr, x0 + gut + 1, tw, rows - vr);
    }
    if (!focused) return;
    g_cy = y0 + max2(0, min2(cursor_row(b, tw), rows - 1));
    if (wrap) {
        int seg = min2(rx / tw, line_rows(b, b->cy, tw) - 1);  /* clamp at EOL */
        g_cx = x0 + gut + 1 + (rx - seg * tw);
    } else {
        g_cx = x0 + gut + 1 + (rx - b->coloff);
    }
}
/* Draw every pane, focused one last so it owns g_wtw and the cursor. */
static void draw_editor(int h, int w) {
    (void)h; (void)w;
    Rect a = editor_area();
    g_cy = g_cx = -1;
    memset(&g_lay, 0, sizeof g_lay);
    g_lay_hdr = 0;
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
    g_lay = L;
    g_lay_hdr = hdr;
    if (L.n == 1) {                       /* too cramped to split, or single */
        draw_pane(a, curpane, cur, 1, 0);
    } else {
        for (int i = 0; i < L.n; i++)
            if (i != focus) draw_pane(L.r[i], i, panes[i], 0, hdr);
        draw_pane(L.r[focus], focus, panes[focus], 1, hdr);
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
        char zoom[48] = "";
        if (b->pdf_img)
            snprintf(zoom, sizeof zoom, "   %d%%   +/- zoom, 0 reset",
                     (int)(p->zoom * 100 + 0.5));
        snprintf(left, sizeof left, " %s   PDF   page %d/%d%s   Left/Right = page"
                 "   v = %s", b->path, p->npg ? p->page + 1 : 0, p->npg, zoom,
                 b->pdf_img ? "text" : "page image");
    } else if (cur >= 0 && tabs[cur]->md_view) {
        Buf *b = tabs[cur];
        Md *m = b->md;
        int pct = (m && m->n > 1) ? m->rowoff * 100 / (m->n - 1) : 100;
        snprintf(left, sizeof left, " %s%s   markdown rendered   %d%%"
                 "   Alt+M = source", b->path, b->dirty ? " [+]" : "", pct);
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
/* ── the help panel ───────────────────────────────────────────────────
 * The help is a list of entries rather than pre-formatted lines, so it can
 * be laid out for the terminal it is being shown on: as many columns as the
 * width allows, wrapped rather than truncated when narrow. Scrolling is the
 * fallback for a window too small to hold it, not the normal case.        */

enum { HE_SEC, HE_KEY, HE_NOTE };
typedef struct { unsigned char kind; const char *a, *b; } HelpEnt;

static const HelpEnt help_ents[] = {
  { HE_SEC,  "FILE TREE", NULL },
  { HE_KEY,  "Alt+Up/Down",      "move in tree" },
  { HE_KEY,  "Alt+Right/Left",   "expand / collapse" },
  { HE_KEY,  "Alt+Enter",        "open / toggle" },
  { HE_KEY,  "Alt+Shift+Enter",  "open in a pane" },
  { HE_KEY,  "Alt+Insert",       "new file / folder" },
  { HE_KEY,  "Alt+Delete",       "delete file / dir" },
  { HE_KEY,  "F5 / Alt+E",       "rescan tree" },
  { HE_KEY,  "Alt+B",            "show / hide sidebar" },
  { HE_NOTE, NULL, "Alt+Left at the top level hides the sidebar; these keys "
                   "need it on screen." },
  { HE_NOTE, NULL, "Marks: M modified · ? new · A added · D deleted" },

  { HE_SEC,  "TABS & PANES", NULL },
  { HE_KEY,  "Alt+, / Alt+.",    "previous / next tab" },
  { HE_KEY,  "Alt+1..9",         "go to tab N" },
  { HE_KEY,  "Alt+W",            "close tab" },
  { HE_KEY,  "Alt+Shift+1..9",   "show tab N in a pane" },
  { HE_KEY,  "Alt+Shift+arrows", "focus a pane" },
  { HE_KEY,  "Alt+Shift+0",      "close this pane" },
  { HE_NOTE, NULL, "Four panes at most, in a 2x2 grid; the same chord on the "
                   "pane you are in folds it away." },

  { HE_SEC,  "EDITING", NULL },
  { HE_KEY,  "Ctrl+Z / Ctrl+Y",  "undo / redo" },
  { HE_KEY,  "Ctrl+C / X / V",   "copy / cut / paste" },
  { HE_KEY,  "Ctrl+A",           "select all" },
  { HE_KEY,  "Ctrl+D",           "duplicate line" },
  { HE_KEY,  "Ctrl+K or Ctrl+/", "toggle comment" },
  { HE_KEY,  "Ctrl+Shift+Up/Dn", "move line" },
  { HE_KEY,  "Alt+O",            "new line below" },
  { HE_KEY,  "Tab / Shift+Tab",  "indent / dedent" },
  { HE_KEY,  "Shift+arrows",     "select" },
  { HE_KEY,  "Ctrl+Left/Right",  "word jump" },
  { HE_KEY,  "Ctrl+Home/End",    "file start / end" },
  { HE_KEY,  "Ctrl+Space",       "autocomplete" },
  { HE_KEY,  "Alt+Z",            "toggle wrap" },
  { HE_KEY,  "Alt+L",            "sideways scroll: line / view" },
  { HE_KEY,  "Esc",              "clear selection / highlight" },

  { HE_SEC,  "FIND & GO", NULL },
  { HE_KEY,  "Ctrl+F",           "find (Enter = next)" },
  { HE_KEY,  "F3",               "find next" },
  { HE_KEY,  "Ctrl+R",           "replace (y/n/a/q)" },
  { HE_KEY,  "Ctrl+G",           "go to line" },
  { HE_KEY,  "Ctrl+P",           "quick-open file" },

  { HE_SEC,  "APP", NULL },
  { HE_KEY,  "Ctrl+S / Alt+S",   "save" },
  { HE_KEY,  "Alt+R",            "run a shell command" },
  { HE_KEY,  "Alt+H",            "this help" },
  { HE_KEY,  "Alt+Q",            "quit" },
  { HE_NOTE, NULL, "Config and themes: ~/.config/sds/" },

  { HE_SEC,  "TERMINAL", NULL },
  { HE_KEY,  "Alt+T",            "new terminal tab" },
  { HE_KEY,  "Shift+PgUp/PgDn",  "scrollback" },
  { HE_NOTE, NULL, "Type 'exit' to end the shell." },

  { HE_SEC,  "ERRORS", NULL },
  { HE_NOTE, NULL, "C/C++ errors and warnings appear as you type, in a gap "
                   "under their line. Click one to jump to it. Needs clangd." },

  { HE_SEC,  "MARKDOWN", NULL },
  { HE_KEY,  "Alt+M",            "rendered view / source" },
  { HE_NOTE, NULL, "The rendered view scrolls with the arrows; editing "
                   "happens in the source." },

  { HE_SEC,  "PDF (read-only)", NULL },
  { HE_KEY,  "Left/Right, n/p",  "previous / next page" },
  { HE_KEY,  "Up/Down, PgUp/Dn", "scroll" },
  { HE_KEY,  "+ / -",            "zoom" },
  { HE_KEY,  "0 / f",            "reset zoom / fit page" },
  { HE_KEY,  "Shift+arrows",     "pan" },
  { HE_KEY,  "v",                "page image / text" },
  { HE_KEY,  "Ctrl+G",           "go to page" },
  { HE_KEY,  "Ctrl+F / Ctrl+C",  "search / copy" },

  { HE_SEC,  "MOUSE", NULL },
  { HE_KEY,  "tab bar",          "click focuses · middle closes" },
  { HE_KEY,  "tree",             "click opens · middle to a pane" },
  { HE_KEY,  "text",             "click places the cursor" },
  { HE_KEY,  "drag",             "select · 2x word · 3x line" },
  { HE_KEY,  "wheel",            "scrolls the pane under it" },
  { HE_NOTE, NULL, "Drag the sidebar edge to resize it. Hold Shift to let "
                   "the terminal have the mouse instead." },
};
#define NHELP ((int)(sizeof help_ents / sizeof *help_ents))

int help_off = 0;        /* first entry shown, when the panel must scroll */
static int help_keyw = 16;   /* the key column's width, measured at draw time */
static int help_shown = 1;   /* entries the last frame had room for */

/* Move the help's window. `page` scrolls by whatever the last frame fitted,
 * so PgUp/PgDn step a screenful however the panel happens to be laid out. */
void help_scroll(int delta, int page) {
    if (page) delta *= max2(1, help_shown);
    help_off = max2(0, help_off + delta);
}

#define HELP_GAP 3       /* blank columns between two columns of entries */

/* Widths here are screen columns, not bytes: the text carries UTF-8 (·, —),
 * and measuring it in bytes makes columns too wide and spacing uneven. */
static int hw(const char *s) { return md_cols(s, (int)strlen(s)); }
/* Word-wrap `s` into a column `w` wide at (y,x) and return the rows it took.
 * With y < 0 it only measures, which is how the layout is worked out before
 * anything is drawn. */
static int help_wrap(const char *s, int y, int x, int w) {
    if (w < 1) return 1;
    int row = 0, used = 0;
    for (const char *p = s; *p; ) {
        int bytes = (int)strcspn(p, " "), word = md_cols(p, bytes);
        if (used && used + 1 + word > w) { row++; used = 0; }
        if (used) { if (y >= 0) mvaddnstr(y + row, x + used, " ", 1); used++; }
        if (y >= 0) mvaddnstr(y + row, x + used, p, bytes);
        used += word;
        p += bytes;
        while (*p == ' ') p++;
    }
    return row + 1;
}
/* Rows `e` takes in a column `w` wide: a key with its (wrapped) description,
 * a wrapped note, and a blank line above every section but the first. */
static int help_rows(const HelpEnt *e, int w, int first) {
    if (e->kind == HE_SEC)  return first ? 1 : 2;
    if (e->kind == HE_KEY)  return help_wrap(e->b, -1, 0, w - help_keyw - 1);
    return help_wrap(e->b, -1, 0, w);
}
/* Draw one entry at (y,x) in a column `w` wide; returns the rows it used. */
static int help_draw_ent(const HelpEnt *e, int y, int x, int w, int keyw, int first) {
    (void)keyw;
    if (e->kind == HE_SEC) {
        int at = first ? y : y + 1;
        attron(A_BOLD);
        mvaddnstr(at, x, e->a, md_byte_at(e->a, (int)strlen(e->a), w));
        attroff(A_BOLD);
        return first ? 1 : 2;
    }
    if (e->kind == HE_KEY) {
        mvaddnstr(y, x, e->a, md_byte_at(e->a, (int)strlen(e->a), w));
        return help_wrap(e->b, y, x + help_keyw + 1, w - help_keyw - 1);
    }
    return help_wrap(e->b, y, x, w);
}
/* Rows the whole list takes in columns `w` wide. */
static int total_help_rows(int w) {
    int total = 0;
    for (int i = 0; i < NHELP; i++) total += help_rows(&help_ents[i], w, i == 0);
    return total;
}
static void draw_help(int h, int w) {
    /* the widest key and the widest line, so columns are as narrow as the
     * content allows and two of them fit on an ordinary terminal */
    int keyw = 0, want = 24;
    for (int i = 0; i < NHELP; i++) {
        const HelpEnt *e = &help_ents[i];
        if (e->kind == HE_KEY) keyw = max2(keyw, hw(e->a));
    }
    help_keyw = keyw;
    for (int i = 0; i < NHELP; i++) {
        const HelpEnt *e = &help_ents[i];
        if (e->kind == HE_KEY)      want = max2(want, keyw + 1 + hw(e->b));
        else if (e->kind == HE_SEC) want = max2(want, hw(e->a));
    }
    int avail_w = max2(20, w - 4), avail_h = max2(4, h - 4);
    /* Columns are sized from what a description needs to stay readable, not
     * from the widest entry: insisting on the widest costs a whole column on
     * a 100-column terminal for the sake of one long line, which then wraps
     * anyway. */
    int minw = min2(want, keyw + 17);
    int ncols = max2(1, min2(4, (avail_w + HELP_GAP) / (minw + HELP_GAP)));
    /* fewer columns when the whole thing already fits in them */
    while (ncols > 1 && total_help_rows(min2(want, (avail_w - (ncols - 2) * HELP_GAP)
                                             / (ncols - 1))) <= (ncols - 1) * avail_h)
        ncols--;
    int colw = min2(want, (avail_w - (ncols - 1) * HELP_GAP) / ncols);

    /* balance the columns: aim for the shortest height that still fits */
    int total = total_help_rows(colw);
    int colh = min2(avail_h, max2(1, (total + ncols - 1) / ncols));
    int fits = total <= colh * ncols;
    if (!fits) colh = avail_h;
    help_off = max2(0, min2(help_off, fits ? 0 : NHELP - 1));

    /* place the entries, column by column */
    struct { int col, row; } at[NHELP];
    int placed = 0, col = 0, row = 0;
    for (int i = help_off; i < NHELP && col < ncols; i++) {
        const HelpEnt *e = &help_ents[i];
        int need = help_rows(e, colw, row == 0);
        /* never leave a section heading stranded at the foot of a column */
        int look = (e->kind == HE_SEC && i + 1 < NHELP)
                 ? need + help_rows(&help_ents[i + 1], colw, 0) : need;
        if (row && row + look > colh) { col++; row = 0; need = help_rows(e, colw, 1); }
        if (col >= ncols) break;
        at[i].col = col;
        at[i].row = row;
        row += need;
        placed = i + 1;
    }
    int used_cols = 0, used_rows = 0;
    for (int i = help_off; i < placed; i++) {
        used_cols = max2(used_cols, at[i].col + 1);
        used_rows = max2(used_rows, at[i].row + help_rows(&help_ents[i], colw,
                                                         at[i].row == 0));
    }
    if (used_cols < 1) used_cols = 1;

    help_shown = max2(1, placed - help_off);
    int more = NHELP - placed;
    int bw = min2(w, used_cols * colw + (used_cols - 1) * HELP_GAP + 4);
    int bh = min2(h, used_rows + (more > 0 ? 4 : 3));
    int y0 = max2(0, (h - bh) / 2), x0 = max2(0, (w - bw) / 2);

    attron(COLOR_PAIR(CP_SEL));
    for (int r = 0; r < bh && y0 + r < h; r++) {
        move(y0 + r, x0);
        for (int c = 0; c < bw && x0 + c < w; c++) addch(' ');
    }
    attron(A_BOLD);
    mvaddnstr(y0, x0 + 2, "sds — keybindings", bw - 4);   /* fits by design */
    attroff(A_BOLD);
    for (int i = help_off; i < placed; i++)
        help_draw_ent(&help_ents[i], y0 + 2 + at[i].row,
                      x0 + 2 + at[i].col * (colw + HELP_GAP), colw, keyw,
                      at[i].row == 0);
    if (more > 0) {
        char line[80];
        snprintf(line, sizeof line, "%d more — Up/Down or the wheel", more);
        attron(A_BOLD);
        mvaddnstr(y0 + bh - 1, x0 + 2, line, bw - 4);
        attroff(A_BOLD);
    }
    attroff(COLOR_PAIR(CP_SEL));
}
void draw(void) {
    int h = LINES, w = COLS;
    gfx_reset_frame();
    erase();
    draw_tabbar(w);
    draw_tree(h);
    /* editor first: it can raise a message (a page that would not render),
     * and the status bar is what shows it */
    draw_editor(h, w);
    draw_status(h, w);
    if (show_help) draw_help(h, w);
    int wantcur = cur >= 0 && !show_help && g_cy >= 0 &&
                  tabs[cur]->kind != TAB_PDF && !tabs[cur]->md_view;
    if (wantcur) move(g_cy, g_cx);
    curs_set(wantcur ? 1 : 0);
    refresh();
    if (show_help) gfx_reset_frame();     /* the help box owns the screen */
    gfx_flush();
}
