#include "core/sds.h"
#include "commands/commands.h"
#include "commands/commands_internal.h"
#include "input/input.h"
#include "editor/editor.h"
#include "ui/ui.h"

char  findq[256] = "";  int find_show = 0;

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
void do_find(void) {
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
void find_next(void) {
    if (cur < 0 || !findq[0]) return;
    Buf *b = tabs[cur];
    int my, mx;
    find_show = 1;
    if (find_from(b, b->cy, b->cx + 1, &my, &mx)) { b->cy = my; b->cx = mx; }
    else set_msg("no match: %s", findq);
}
void do_replace(void) {
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
void do_goto(void) {
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
