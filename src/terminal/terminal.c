#include "core/sds.h"
#include "terminal/terminal.h"
#include "terminal/terminal_internal.h"
#include "input/input.h"
#include "syntax/syntax.h"
#include "ui/ui.h"
#include "tree/tree.h"

/* ── terminal lifecycle ───────────────────────────────────────────── */
void term_size(Term *t, int rows, int cols) {
    if (rows < 1) rows = 1;
    if (cols < 1) cols = 1;
    if (t->g && rows == t->rows && cols == t->cols) return;

    Cell *ng = xmalloc((size_t)rows * cols * sizeof(Cell));
    Cell blank = term_blank(t);
    for (int i = 0; i < rows * cols; i++) ng[i] = blank;
    if (t->g) {                             /* keep what still fits */
        int keep = min2(rows, t->rows);
        int srcy = t->rows - keep, dsty = rows - keep;
        for (int y = 0; y < keep; y++)
            memcpy(ng + (size_t)(dsty + y) * cols,
                   t->g + (size_t)(srcy + y) * t->cols,
                   (size_t)min2(cols, t->cols) * sizeof(Cell));
        free(t->g);
        t->cy = max2(0, min2(rows - 1, t->cy - srcy));
    }
    t->g = ng;
    free(t->alt); t->alt = NULL;            /* rebuilt on next use */
    t->alt_on = 0;

    /* the scrollback ring is row-width-sensitive; simplest correct thing on a
     * width change is to start it over rather than reflow */
    if (!t->sb || cols != t->cols) {
        free(t->sb);
        t->sb = xmalloc((size_t)TERM_SB_MAX * cols * sizeof(Cell));
        for (int i = 0; i < TERM_SB_MAX * cols; i++) t->sb[i] = blank;
        t->sb_n = t->sb_head = t->sb_view = 0;
    }
    t->rows = rows; t->cols = cols;
    t->top = 0; t->bot = rows - 1;
    t->cx = min2(t->cx, cols - 1);
    t->cy = min2(t->cy, rows - 1);
    if (t->fd >= 0) {
        struct winsize ws = { (unsigned short)rows, (unsigned short)cols, 0, 0 };
        ioctl(t->fd, TIOCSWINSZ, &ws);
    }
}
static Term *term_new(int rows, int cols, const char *cwd) {
    Term *t = calloc(1, sizeof *t);
    if (!t) die("out of memory");
    t->fd = -1; t->fg = t->bg = -1;
    t->rows = t->cols = 0;
    snprintf(t->title, sizeof t->title, "shell");
    term_size(t, rows, cols);

    struct winsize ws = { (unsigned short)rows, (unsigned short)cols, 0, 0 };
    pid_t pid = forkpty(&t->fd, NULL, NULL, &ws);
    if (pid < 0) { free(t->g); free(t->sb); free(t); return NULL; }
    if (pid == 0) {                          /* child: become the shell */
        if (cwd) { if (chdir(cwd) != 0) { /* stay put */ } }
        setenv("TERM", "xterm-256color", 1);
        unsetenv("LINES"); unsetenv("COLUMNS");
        const char *sh = getenv("SHELL");
        if (!sh || !*sh) sh = "/bin/sh";
        execl(sh, sh, "-i", (char *)NULL);
        execl("/bin/sh", "sh", (char *)NULL);
        _exit(127);
    }
    t->pid = pid;
    fcntl(t->fd, F_SETFL, O_NONBLOCK);
    return t;
}
void term_free(Term *t) {
    if (!t) return;
    if (t->fd >= 0) close(t->fd);
    if (t->pid > 0) {
        kill(t->pid, SIGHUP);
        waitpid(t->pid, NULL, WNOHANG);
    }
    free(t->g); free(t->alt); free(t->sb);
    free(t);
}
/* Drain whatever the shell has produced. Returns 1 if the screen changed. */
static int term_pump(Term *t) {
    if (!t || t->fd < 0 || t->dead) return 0;
    char buf[8192];
    int changed = 0;
    for (int guard = 0; guard < 64; guard++) {   /* bounded: stay responsive */
        ssize_t n = read(t->fd, buf, sizeof buf);
        if (n > 0) { term_feed(t, buf, (int)n); changed = 1; continue; }
        if (n == 0) { t->dead = 1; changed = 1; break; }
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        if (errno == EINTR) continue;
        t->dead = 1; changed = 1; break;         /* EIO: child exited */
    }
    if (t->dead && t->pid > 0) {
        int st = 0;
        if (waitpid(t->pid, &st, WNOHANG) == t->pid) { t->status = st; t->pid = 0; }
    }
    return changed;
}
static void term_write(Term *t, const char *s, int n) {
    if (!t || t->fd < 0 || t->dead) return;
    while (n > 0) {
        ssize_t w = write(t->fd, s, (size_t)n);
        if (w <= 0) { if (errno == EINTR) continue; break; }
        s += w; n -= (int)w;
    }
}
/* Translate an sds key code into the bytes a real terminal would send. */
void term_key(Term *t, int c) {
    char b[16];
    t->sb_view = 0;                       /* any keypress jumps back to live */
    switch (c) {
        case KEY_UP:    term_write(t, "\033[A", 3); return;
        case KEY_DOWN:  term_write(t, "\033[B", 3); return;
        case KEY_RIGHT: term_write(t, "\033[C", 3); return;
        case KEY_LEFT:  term_write(t, "\033[D", 3); return;
        case KEY_HOME:  term_write(t, "\033[H", 3); return;
        case KEY_END:   term_write(t, "\033[F", 3); return;
        case KEY_PPAGE: term_write(t, "\033[5~", 4); return;
        case KEY_NPAGE: term_write(t, "\033[6~", 4); return;
        case KEY_DC:    term_write(t, "\033[3~", 4); return;
        case KEY_IC:    term_write(t, "\033[2~", 4); return;
        case KEY_BTAB:  term_write(t, "\033[Z", 3); return;
        case KEY_BACKSPACE: case 127: case 8: term_write(t, "\177", 1); return;
        case '\r': case '\n': case KEY_ENTER: term_write(t, "\r", 1); return;
        default: break;
    }
    if (c >= KEY_F(1) && c <= KEY_F(12)) {
        static const char *fk[12] = {
            "\033OP", "\033OQ", "\033OR", "\033OS", "\033[15~", "\033[17~",
            "\033[18~", "\033[19~", "\033[20~", "\033[21~", "\033[23~", "\033[24~"
        };
        const char *s = fk[c - KEY_F(1)];
        term_write(t, s, (int)strlen(s));
        return;
    }
    if (c >= ALT(0) && c <= ALT(255)) {          /* Alt+x → ESC x */
        b[0] = 27; b[1] = (char)(c - ALT(0));
        term_write(t, b, 2);
        return;
    }
    if (c >= 0 && c < 256) { b[0] = (char)c; term_write(t, b, 1); }
}
/* Open a shell in a new tab, sized to the current editor pane. */
void open_terminal(void) {
    if (ntabs == MAX_TABS) { set_msg("too many open tabs", NULL); return; }
    int x0 = tree_hidden ? 0 : tree_w + 1;
    int rows = max2(1, LINES - 2), cols = max2(1, COLS - x0);
    Term *t = term_new(rows, cols, root ? root->path : NULL);
    if (!t) { set_msg("could not start a shell", NULL); return; }

    Buf *b = calloc(1, sizeof *b);
    if (!b) die("out of memory");
    b->kind = TAB_TERM;
    b->term = t;
    b->lang = LANG_TEXT;
    /* one empty line so any stray editor-side code still sees a sane buffer */
    b->ln = xmalloc(sizeof(Line));
    b->ln[0].s = xmalloc(1); b->ln[0].s[0] = 0;
    b->ln[0].len = 0; b->ln[0].cap = 1; b->ln[0].hst = 0;
    b->n = b->cap = 1;
    snprintf(b->name, sizeof b->name, "shell");
    snprintf(b->path, sizeof b->path, "%s", root ? root->path : "");
    tabs[ntabs++] = b;
    set_cur(ntabs - 1);
    set_msg("terminal opened — type 'exit' to close", NULL);
}
/* Any live terminal means the main loop must poll rather than block. */
int any_live_term(void) {
    for (int i = 0; i < ntabs; i++)
        if (tabs[i]->kind == TAB_TERM && tabs[i]->term && !tabs[i]->term->dead)
            return 1;
    return 0;
}
int pump_all_terms(void) {
    int changed = 0;
    for (int i = 0; i < ntabs; i++)
        if (tabs[i]->kind == TAB_TERM) changed |= term_pump(tabs[i]->term);
    return changed;
}
