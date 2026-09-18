#include "core/sds.h"
#include "editor/editor.h"
#include "editor/editor_internal.h"
#include "input/input.h"
#include "ui/ui.h"
#include "tree/tree.h"

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
void do_complete(void) {
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
