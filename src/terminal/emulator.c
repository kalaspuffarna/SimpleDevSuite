#include "core/sds.h"
#include "terminal/terminal.h"
#include "terminal/terminal_internal.h"
#include "ui/ui.h"

enum { PS_GROUND, PS_ESC, PS_CSI, PS_OSC, PS_CHARSET };

Cell term_blank(Term *t) {
    Cell c;
    c.b[0] = ' '; c.b[1] = c.b[2] = c.b[3] = 0;
    c.n = 1; c.at = 0; c.fg = -1; c.bg = t ? t->bg : -1;
    return c;
}
Cell *term_row(Term *t, int y) { return t->g + (size_t)y * t->cols; }
static void term_clear_row(Term *t, int y, int from, int to) {
    if (y < 0 || y >= t->rows) return;
    Cell *r = term_row(t, y);
    Cell b = term_blank(t);
    for (int x = max2(0, from); x <= to && x < t->cols; x++) r[x] = b;
}
/* the line scrolling off the top of the region is kept for scrollback */
static void term_to_scrollback(Term *t, int y) {
    if (!t->sb || t->alt_on) return;
    memcpy(t->sb + (size_t)t->sb_head * t->cols, term_row(t, y),
           (size_t)t->cols * sizeof(Cell));
    t->sb_head = (t->sb_head + 1) % TERM_SB_MAX;
    if (t->sb_n < TERM_SB_MAX) t->sb_n++;
}
static void term_scroll_up(Term *t, int n) {
    for (int k = 0; k < n; k++) {
        term_to_scrollback(t, t->top);
        for (int y = t->top; y < t->bot; y++)
            memcpy(term_row(t, y), term_row(t, y + 1), (size_t)t->cols * sizeof(Cell));
        term_clear_row(t, t->bot, 0, t->cols - 1);
    }
}
static void term_scroll_down(Term *t, int n) {
    for (int k = 0; k < n; k++) {
        for (int y = t->bot; y > t->top; y--)
            memcpy(term_row(t, y), term_row(t, y - 1), (size_t)t->cols * sizeof(Cell));
        term_clear_row(t, t->top, 0, t->cols - 1);
    }
}
static void term_newline(Term *t) {
    if (t->cy == t->bot) term_scroll_up(t, 1);
    else if (t->cy < t->rows - 1) t->cy++;
}
static void term_put(Term *t, const char *b, int n) {
    if (t->wrapnext) { t->cx = 0; term_newline(t); t->wrapnext = 0; }
    if (t->cx >= t->cols) t->cx = t->cols - 1;
    Cell *c = &term_row(t, t->cy)[t->cx];
    int k = n > 4 ? 4 : n;
    memcpy(c->b, b, (size_t)k);
    c->n = (unsigned char)k;
    c->at = t->at; c->fg = t->fg; c->bg = t->bg;
    if (t->cx + 1 >= t->cols) t->wrapnext = 1;      /* defer the wrap, like xterm */
    else t->cx++;
}
static void term_sgr(Term *t) {
    if (!t->np) { t->np = 1; t->params[0] = 0; }
    for (int i = 0; i < t->np; i++) {
        int p = t->params[i];
        if (p == 0)      { t->at = 0; t->fg = -1; t->bg = -1; }
        else if (p == 1) t->at |= TA_BOLD;
        else if (p == 2) t->at |= TA_DIM;
        else if (p == 4) t->at |= TA_UNDER;
        else if (p == 7) t->at |= TA_REV;
        else if (p == 22) t->at &= (unsigned char)~(TA_BOLD | TA_DIM);
        else if (p == 24) t->at &= (unsigned char)~TA_UNDER;
        else if (p == 27) t->at &= (unsigned char)~TA_REV;
        else if (p >= 30 && p <= 37)   t->fg = (short)(p - 30);
        else if (p >= 40 && p <= 47)   t->bg = (short)(p - 40);
        else if (p >= 90 && p <= 97)   t->fg = (short)(p - 90 + 8);
        else if (p >= 100 && p <= 107) t->bg = (short)(p - 100 + 8);
        else if (p == 39) t->fg = -1;
        else if (p == 49) t->bg = -1;
        else if ((p == 38 || p == 48) && i + 1 < t->np) {
            short v = -1;
            if (t->params[i + 1] == 5 && i + 2 < t->np) {
                v = (short)t->params[i + 2]; i += 2;
            } else if (t->params[i + 1] == 2 && i + 4 < t->np) {
                int rgb = (t->params[i+2] << 16) | (t->params[i+3] << 8) | t->params[i+4];
                v = (short)rgb_to_256(rgb);        /* fold truecolor to 256 */
                i += 4;
            }
            if (v >= 0) { if (p == 38) t->fg = v; else t->bg = v; }
        }
    }
}
static void term_use_alt(Term *t, int on) {
    if (on == t->alt_on) return;
    if (!t->alt) {
        t->alt = xmalloc((size_t)t->rows * t->cols * sizeof(Cell));
        Cell b = term_blank(t);
        for (int i = 0; i < t->rows * t->cols; i++) t->alt[i] = b;
    }
    Cell *tmp = t->g; t->g = t->alt; t->alt = tmp;
    t->alt_on = on;
    if (on) {
        Cell b = term_blank(t);
        for (int i = 0; i < t->rows * t->cols; i++) t->g[i] = b;
        t->cy = t->cx = 0;
    }
}
static void term_csi(Term *t, char f) {
    int p0 = t->np > 0 ? t->params[0] : 0;
    int p1 = t->np > 1 ? t->params[1] : 0;
    int n  = p0 ? p0 : 1;
    switch (f) {
        case 'A': t->cy = max2(t->top, t->cy - n); t->wrapnext = 0; break;
        case 'B': t->cy = min2(t->bot, t->cy + n); t->wrapnext = 0; break;
        case 'C': t->cx = min2(t->cols - 1, t->cx + n); t->wrapnext = 0; break;
        case 'D': t->cx = max2(0, t->cx - n); t->wrapnext = 0; break;
        case 'E': t->cy = min2(t->bot, t->cy + n); t->cx = 0; break;
        case 'F': t->cy = max2(t->top, t->cy - n); t->cx = 0; break;
        case 'G': t->cx = min2(t->cols - 1, max2(0, n - 1)); t->wrapnext = 0; break;
        case 'd': t->cy = min2(t->rows - 1, max2(0, n - 1)); t->wrapnext = 0; break;
        case 'H': case 'f':
            t->cy = min2(t->rows - 1, max2(0, (p0 ? p0 : 1) - 1));
            t->cx = min2(t->cols - 1, max2(0, (p1 ? p1 : 1) - 1));
            t->wrapnext = 0;
            break;
        case 'J':
            /* ED 3 drops the scrollback and leaves the screen alone. `clear`
             * sends it right after the 2J, and without it everything it just
             * "cleared" is still there a Shift+PgUp away. */
            if (p0 == 3) {
                t->sb_n = t->sb_head = t->sb_view = 0;
            } else if (p0 == 0) {
                term_clear_row(t, t->cy, t->cx, t->cols - 1);
                for (int y = t->cy + 1; y < t->rows; y++) term_clear_row(t, y, 0, t->cols - 1);
            } else if (p0 == 1) {
                term_clear_row(t, t->cy, 0, t->cx);
                for (int y = 0; y < t->cy; y++) term_clear_row(t, y, 0, t->cols - 1);
            } else {
                for (int y = 0; y < t->rows; y++) term_clear_row(t, y, 0, t->cols - 1);
            }
            break;
        case 'K':
            if (p0 == 0)      term_clear_row(t, t->cy, t->cx, t->cols - 1);
            else if (p0 == 1) term_clear_row(t, t->cy, 0, t->cx);
            else              term_clear_row(t, t->cy, 0, t->cols - 1);
            break;
        case 'L': { int s = t->top; t->top = t->cy;      /* insert lines */
                    term_scroll_down(t, min2(n, t->bot - t->cy + 1)); t->top = s; break; }
        case 'M': { int s = t->top; t->top = t->cy;      /* delete lines */
                    term_scroll_up(t, min2(n, t->bot - t->cy + 1)); t->top = s; break; }
        case 'P': {                                     /* delete chars */
            Cell *r = term_row(t, t->cy);
            int k = min2(n, t->cols - t->cx);
            memmove(r + t->cx, r + t->cx + k,
                    (size_t)(t->cols - t->cx - k) * sizeof(Cell));
            term_clear_row(t, t->cy, t->cols - k, t->cols - 1);
            break;
        }
        case '@': {                                     /* insert blanks */
            Cell *r = term_row(t, t->cy);
            int k = min2(n, t->cols - t->cx);
            memmove(r + t->cx + k, r + t->cx,
                    (size_t)(t->cols - t->cx - k) * sizeof(Cell));
            term_clear_row(t, t->cy, t->cx, t->cx + k - 1);
            break;
        }
        case 'X': term_clear_row(t, t->cy, t->cx, min2(t->cols - 1, t->cx + n - 1)); break;
        case 'S': term_scroll_up(t, n);   break;
        case 'T': term_scroll_down(t, n); break;
        case 'r':
            t->top = max2(0, (p0 ? p0 : 1) - 1);
            t->bot = min2(t->rows - 1, (p1 ? p1 : t->rows) - 1);
            if (t->top >= t->bot) { t->top = 0; t->bot = t->rows - 1; }
            t->cy = t->top; t->cx = 0;
            break;
        /* CSI > 4 ; n m sets modifyOtherKeys; it is not SGR, and reading it as
         * one would turn the rest of the output bold and underlined. */
        case 'm': if (!t->priv) term_sgr(t); break;
        case 's': t->scy = t->cy; t->scx = t->cx; break;
        case 'u': t->cy = t->scy; t->cx = t->scx; break;
        case 'h': case 'l':
            if (t->priv) {
                int on = (f == 'h');
                if (p0 == 25) t->hidecur = !on;
                else if (p0 == 1049 || p0 == 47 || p0 == 1047) term_use_alt(t, on);
            }
            break;
        default: break;
    }
}
/* run bytes from the pty through the parser */
void term_feed(Term *t, const char *s, int n) {
    for (int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (t->ps) {
            case PS_GROUND:
                if (c == 0x1b) { t->ps = PS_ESC; t->np = 0; t->priv = 0; t->oscn = 0; }
                else if (c == '\n' || c == 0x0b || c == 0x0c) { t->wrapnext = 0; term_newline(t); }
                else if (c == '\r') { t->cx = 0; t->wrapnext = 0; }
                else if (c == '\b') { if (t->cx > 0) t->cx--; t->wrapnext = 0; }
                else if (c == '\t') {
                    t->cx = min2(t->cols - 1, (t->cx / 8 + 1) * 8);
                    t->wrapnext = 0;
                }
                else if (c < 32 || c == 127) { /* other C0: ignore */ }
                else if (c < 0x80) { char b = (char)c; term_put(t, &b, 1); }
                else {
                    /* gather a UTF-8 sequence so it lands in one cell */
                    if (t->pendn == 0) {
                        t->uexp = (c >= 0xf0) ? 4 : (c >= 0xe0) ? 3 : 2;
                        t->pend[0] = (char)c; t->pendn = 1;
                    } else if (t->pendn < 4) {
                        t->pend[t->pendn++] = (char)c;
                    }
                    if (t->pendn >= t->uexp) { term_put(t, t->pend, t->pendn); t->pendn = 0; }
                }
                break;
            case PS_ESC:
                if (c == '[') { t->ps = PS_CSI; t->np = 0; t->params[0] = 0; t->priv = 0; }
                else if (c == ']') { t->ps = PS_OSC; t->oscn = 0; }
                else if (c == '(' || c == ')' || c == '*' || c == '+') t->ps = PS_CHARSET;
                else {
                    if (c == 'M') {                     /* reverse index */
                        if (t->cy == t->top) term_scroll_down(t, 1);
                        else if (t->cy > 0) t->cy--;
                    } else if (c == '7') { t->scy = t->cy; t->scx = t->cx; }
                    else if (c == '8') { t->cy = t->scy; t->cx = t->scx; }
                    else if (c == 'c') {                /* reset */
                        t->at = 0; t->fg = t->bg = -1;
                        t->top = 0; t->bot = t->rows - 1;
                        for (int y = 0; y < t->rows; y++) term_clear_row(t, y, 0, t->cols - 1);
                        t->cy = t->cx = 0;
                    }
                    t->ps = PS_GROUND;
                }
                break;
            case PS_CHARSET: t->ps = PS_GROUND; break;
            case PS_CSI:
                if (c == '?' || c == '>' || c == '!' || c == '$' || c == '"' || c == '\'')
                    t->priv = 1;
                else if (isdigit(c)) {
                    if (t->np == 0) t->np = 1;
                    if (t->np <= 8) t->params[t->np - 1] = t->params[t->np - 1] * 10 + (c - '0');
                } else if (c == ';' || c == ':') {
                    if (t->np == 0) t->np = 1;
                    if (t->np < 8) t->params[t->np++] = 0;
                } else if (c >= 0x40 && c <= 0x7e) {
                    term_csi(t, (char)c);
                    t->ps = PS_GROUND;
                }
                break;
            case PS_OSC:
                if (c == 7 || c == 0x1b) {              /* BEL or start of ST */
                    t->osc[t->oscn] = 0;
                    if ((t->osc[0] == '0' || t->osc[0] == '2') && t->osc[1] == ';')
                        snprintf(t->title, sizeof t->title, "%s", t->osc + 2);
                    t->ps = PS_GROUND;
                } else if (t->oscn + 1 < (int)sizeof t->osc) {
                    t->osc[t->oscn++] = (char)c;
                }
                break;
        }
    }
}
