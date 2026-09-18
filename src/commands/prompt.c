#include "core/sds.h"
#include "commands/commands.h"
#include "commands/commands_internal.h"
#include "input/input.h"
#include "ui/ui.h"

/* ── prompt ───────────────────────────────────────────────────────── */
/* ── command history ──────────────────────────────────────────────── */
#define HIST_MAX 200
static char **rhist = NULL;
static int    nrhist = 0;

static void hist_path(char *out, size_t cap) {
    const char *home = getenv("HOME");
    snprintf(out, cap, "%s/.sds_history", home && *home ? home : ".");
}
void hist_add(const char *s) {
    if (!s || !*s) return;
    if (nrhist && !strcmp(rhist[nrhist - 1], s)) return;   /* no dupes in a row */
    if (nrhist == HIST_MAX) {
        free(rhist[0]);
        memmove(rhist, rhist + 1, (size_t)(nrhist - 1) * sizeof(char *));
        nrhist--;
    }
    rhist = xrealloc(rhist, (size_t)(nrhist + 1) * sizeof(char *));
    rhist[nrhist++] = xstrdup(s);
}
void hist_load(void) {
    char p[PATH_MAX];
    hist_path(p, sizeof p);
    FILE *f = fopen(p, "r");
    if (!f) return;
    char *ln = NULL; size_t cap = 0; ssize_t r;
    while ((r = getline(&ln, &cap, f)) != -1) {
        while (r > 0 && (ln[r-1] == '\n' || ln[r-1] == '\r')) ln[--r] = 0;
        if (r) hist_add(ln);
    }
    free(ln);
    fclose(f);
}
void hist_save(void) {
    if (!nrhist) return;
    char p[PATH_MAX];
    hist_path(p, sizeof p);
    FILE *f = fopen(p, "w");
    if (!f) return;
    for (int i = 0; i < nrhist; i++) fprintf(f, "%s\n", rhist[i]);
    fclose(f);
}

/* Status-bar line editor. `use_hist` enables Up/Down recall of past
 * commands; the half-typed line is kept as a draft below the newest entry. */
int prompt(const char *label, char *out, size_t cap,
                  void (*live)(const char *), int use_hist) {
    size_t n = strlen(out);
    int hidx = nrhist;
    char draft[512] = "";
    draw();                       /* the app behind the prompt, once */
    for (;;) {
        /* only the prompt line is repainted per keystroke; the editor behind
         * it is redrawn solely when a live callback changes its highlighting */
        attron(COLOR_PAIR(CP_STATUS) | A_BOLD);
        move(LINES - 1, 0);
        for (int i = 0; i < COLS; i++) addch(' ');
        mvprintw(LINES - 1, 0, " %s%s", label, out);
        attroff(COLOR_PAIR(CP_STATUS) | A_BOLD);
        curs_set(1);
        refresh();
        int c = read_key();
        if (c == 27) return 0;
        if (c == '\r' || c == '\n' || c == KEY_ENTER) return 1;
        if (c == KEY_RESIZE) { draw(); continue; }
        if (use_hist && (c == KEY_UP || c == KEY_DOWN)) {
            if (c == KEY_UP && hidx > 0) {
                if (hidx == nrhist) snprintf(draft, sizeof draft, "%s", out);
                snprintf(out, cap, "%s", rhist[--hidx]);
            } else if (c == KEY_DOWN && hidx < nrhist) {
                if (++hidx == nrhist) snprintf(out, cap, "%s", draft);
                else                  snprintf(out, cap, "%s", rhist[hidx]);
            }
            n = strlen(out);
            continue;
        }
        if (c == KEY_BACKSPACE || c == 127 || c == 8) {
            if (n) out[--n] = 0;
        } else if (c >= 32 && c < 256 && c != 127 && n + 1 < cap) {
            out[n++] = (char)c;
            out[n] = 0;
        } else continue;
        if (live) { live(out); draw(); }
    }
}
