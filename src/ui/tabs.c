#include "core/sds.h"
#include "ui/ui.h"
#include "ui/ui_internal.h"
#include "editor/editor.h"
#include "pdf/pdf.h"
#include "markdown/markdown.h"
#include "lsp/lsp.h"

Buf  *tabs[MAX_TABS];
int   ntabs = 0, cur = -1;

/* ── tabs ─────────────────────────────────────────────────────────── */
/* Point the focused pane at tab i. The one place `cur` is allowed to move. */
void set_cur(int i) {
    if (i < 0 || i >= ntabs) return;
    cur = panes[curpane] = i;
}
/* Focus tab i, preferring a pane that already shows it over stealing the
 * focused one — otherwise Ctrl+P on a file you can already see would yank it
 * out from under the pane it lives in. */
void focus_tab(int i) {
    if (i < 0 || i >= ntabs) return;
    for (int p = 0; p < npanes; p++)
        if (panes[p] == i) { curpane = p; cur = i; return; }
    set_cur(i);
}
void open_file(const char *path) {
    for (int i = 0; i < ntabs; i++)
        if (tabs[i]->kind != TAB_TERM && strcmp(tabs[i]->path, path) == 0) {
            focus_tab(i);
            return;
        }
    if (ntabs == MAX_TABS) { set_msg("too many open tabs", NULL); return; }
    Buf *b = is_pdf_path(path) ? pdf_load(path) : buf_load(path);
    if (!b) { set_msg("can't open %s", path); return; }
    if (b->kind != TAB_PDF) b->kind = TAB_FILE;
    if (cfg_md_preview && md_is_md(b)) b->md_view = 1;
    tabs[ntabs++] = b;
    set_cur(ntabs - 1);
    lsp_attach(b);                     /* live diagnostics, where a server exists */
}
void close_tab(int i) {
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
void pane_close(void) {
    if (npanes < 2) { set_msg("only one pane open", NULL); return; }
    memmove(panes + curpane, panes + curpane + 1,
            (size_t)(npanes - curpane - 1) * sizeof *panes);
    panes[--npanes] = -1;
    if (curpane >= npanes) curpane = npanes - 1;
    cur = panes[curpane];
}
/* Alt+Shift+N: put tab N in a pane. Already visible elsewhere → focus it;
 * already in *this* pane → fold the pane away, so the same chord toggles. */
void pane_show_tab(int t) {
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
