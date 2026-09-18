#include "core/sds.h"
#include "ui/ui.h"
#include "ui/ui_internal.h"
#include "input/input.h"

/* ── confirm dialog ───────────────────────────────────────────────── */
/* Modal yes/no. `danger` paints the box red-ish and defaults to No.
 * y / n / Enter / arrows / Tab / Esc all behave as you'd expect.        */
/* Popups paint the app behind them exactly once and then only repaint their
 * own rectangle per keystroke. Calling draw() in the input loop was visibly
 * slow on large files — it re-lexes and re-renders every on-screen line for
 * each character typed — and that is what made the whole screen appear to
 * flicker while typing into a dialog. */
int confirm(const char *title, const char *detail, int danger) {
    int yes = !danger;                       /* destructive → default No */
    draw();
    for (;;) {
        int tl = (int)strlen(title), dl = detail ? (int)strlen(detail) : 0;
        int bw = max2(max2(tl, dl) + 6, 34);
        bw = min2(bw, COLS - 2);
        int bh = detail ? 7 : 6;
        int y0 = max2(0, (LINES - bh) / 2), x0 = max2(0, (COLS - bw) / 2);
        int pair = danger ? CP_ERR : CP_STATUS;

        attron(COLOR_PAIR(pair));
        for (int r = 0; r < bh && y0 + r < LINES; r++) {
            move(y0 + r, x0);
            for (int i = 0; i < bw && x0 + i < COLS; i++) addch(' ');
        }
        attron(A_BOLD);
        mvaddnstr(y0 + 1, x0 + 2, title, bw - 4);
        attroff(A_BOLD);
        if (detail) mvaddnstr(y0 + 2, x0 + 2, detail, bw - 4);
        attroff(COLOR_PAIR(pair));

        /* buttons, right-aligned */
        int by = y0 + bh - 2, bx = x0 + bw - 20;
        for (int i = 0; i < 2; i++) {
            const char *lab = i ? "  Yes  " : "  No   ";
            int on = (i == yes);
            attron(COLOR_PAIR(on ? CP_SEL : pair));
            if (on) attron(A_BOLD | A_REVERSE);
            mvaddstr(by, bx + i * 9, lab);
            if (on) attroff(A_BOLD | A_REVERSE);
            attroff(COLOR_PAIR(on ? CP_SEL : pair));
        }
        curs_set(0);
        refresh();

        int c = read_key();
        switch (c) {
            case 'y': case 'Y':                     return 1;
            case 'n': case 'N': case 27:            return 0;
            case KEY_LEFT:  case MK(3, D_LEFT):     yes = 0; break;
            case KEY_RIGHT: case MK(3, D_RIGHT):    yes = 1; break;
            case '\t':                              yes = !yes; break;
            case '\r': case '\n': case KEY_ENTER:   return yes;
            case KEY_RESIZE:  draw();               break;  /* geometry moved */
        }
    }
}

/* ── text input dialog ────────────────────────────────────────────── */
/* Centered single-line editor. Returns 1 on Enter, 0 on Esc. */
int input_box(const char *title, const char *hint, char *out, size_t cap) {
    size_t n = strlen(out);
    draw();                       /* the app behind the dialog, once */
    for (;;) {
        int bw = min2(max2((int)strlen(title) + 6, 46), COLS - 2);
        int bh = hint ? 6 : 5;
        int y0 = max2(0, (LINES - bh) / 2), x0 = max2(0, (COLS - bw) / 2);
        attron(COLOR_PAIR(CP_STATUS));
        for (int r = 0; r < bh && y0 + r < LINES; r++) {
            move(y0 + r, x0);
            for (int i = 0; i < bw && x0 + i < COLS; i++) addch(' ');
        }
        attron(A_BOLD);
        mvaddnstr(y0 + 1, x0 + 2, title, bw - 4);
        attroff(A_BOLD);
        if (hint) mvaddnstr(y0 + bh - 1, x0 + 2, hint, bw - 4);
        attroff(COLOR_PAIR(CP_STATUS));

        int fw = bw - 4;
        attron(COLOR_PAIR(CP_SEL));
        move(y0 + 3, x0 + 2);
        for (int i = 0; i < fw && x0 + 2 + i < COLS; i++) addch(' ');
        int off = max2(0, (int)n - fw + 1);
        mvaddnstr(y0 + 3, x0 + 2, out + off, fw);
        attroff(COLOR_PAIR(CP_SEL));
        curs_set(1);
        move(y0 + 3, x0 + 2 + min2((int)n - off, fw - 1));
        refresh();

        int c = read_key();
        if (c == 27) return 0;
        if (c == '\r' || c == '\n' || c == KEY_ENTER) return n > 0;
        if (c == KEY_RESIZE) { draw(); continue; }
        if (c == KEY_BACKSPACE || c == 127 || c == 8) { if (n) out[--n] = 0; }
        else if (c >= 32 && c < 256 && c != 127 && n + 1 < cap) {
            out[n++] = (char)c;
            out[n] = 0;
        }
    }
}
