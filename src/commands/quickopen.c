#include "core/sds.h"
#include "commands/commands.h"
#include "commands/commands_internal.h"
#include "input/input.h"
#include "ui/ui.h"
#include "tree/tree.h"

/* ── quick open (Ctrl+P) ──────────────────────────────────────────── */
static char **qofiles = NULL;
static int    nqo = 0, qocap = 0;
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
        int isdir = dirent_is_dir(e, p);
        if (isdir < 0) continue;
        if (isdir) qo_walk(p, r, depth + 1);
        else {
            if (nqo == qocap) {        /* doubling, not one realloc per file */
                qocap = qocap ? qocap * 2 : 256;
                qofiles = xrealloc(qofiles, (size_t)qocap * sizeof(char *));
            }
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
void do_quickopen(void) {
    for (int i = 0; i < nqo; i++) free(qofiles[i]);
    free(qofiles); qofiles = NULL; nqo = qocap = 0;
    qo_walk(root->path, "", 0);
    char q[128] = "";
    int seln = 0;
    int *idx = xmalloc((size_t)max2(nqo, 1) * sizeof(int));
    int *scr = xmalloc((size_t)max2(nqo, 1) * sizeof(int));
    draw();                       /* the app behind the picker, once */
    char prevq[128] = "";
    int nprev = 0, have_prev = 0;
    for (;;) {
        /* Adding a character can only narrow the set — a name that does not
         * contain "ab" cannot contain "abc" — so a longer query re-scores
         * what survived rather than the whole tree. Backspacing, or any
         * other change, starts from everything again. */
        size_t pl = strlen(prevq);
        int grew = have_prev && strlen(q) > pl && strncmp(q, prevq, pl) == 0;
        int nm = 0;
        if (grew) {
            for (int k = 0; k < nprev; k++) {
                int s = fuzzy_score(qofiles[idx[k]], q);
                if (s > 0) { idx[nm] = idx[k]; scr[nm] = s; nm++; }
            }
        } else {
            for (int i = 0; i < nqo; i++) {
                int s = fuzzy_score(qofiles[i], q);
                if (s > 0) { idx[nm] = i; scr[nm] = s; nm++; }
            }
        }
        snprintf(prevq, sizeof prevq, "%s", q);
        nprev = nm;
        have_prev = 1;
        int bw = min2(COLS - 4, 64), y0 = 2;
        /* fixed height: the panel neither jumps as you type nor leaves stale
         * rows behind, which matters now that the app is not redrawn under it */
        int lh = min2(12, max2(LINES - y0 - 3, 1));
        /* Only the rows on screen are ever read, so pull the best `lh` to the
         * front rather than ordering all of them: sorting every match cost
         * ~20ms per keystroke in a ten-thousand-file tree, and all but a
         * dozen of that work was thrown away. Ties keep the order the walk
         * found them in. */
        for (int r = 0; r < lh && r < nm; r++) {
            int best = r;
            for (int i = r + 1; i < nm; i++) if (scr[i] > scr[best]) best = i;
            if (best != r) {
                int ti = idx[r], ts = scr[r];
                idx[r] = idx[best]; scr[r] = scr[best];
                idx[best] = ti;     scr[best] = ts;
            }
        }
        if (seln >= nm) seln = max2(0, nm - 1);
        int x0 = (COLS - bw) / 2;
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
        if (c == KEY_RESIZE) { draw(); continue; }
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
