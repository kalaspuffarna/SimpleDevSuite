#include "core/sds.h"
#include "editor/editor.h"
#include "editor/editor_internal.h"
#include "ui/ui.h"

/* ── movement ─────────────────────────────────────────────────────── */
/* Editing is byte-based, but the cursor must not land inside a multi-byte
 * character or a single arrow press would split it and the next edit would
 * corrupt the text. Continuation bytes are 10xxxxxx. */
int utf8_cont(unsigned char c) { return (c & 0xc0) == 0x80; }
int utf8_prev(Line *l, int cx) {
    if (cx <= 0) return 0;
    cx--;
    while (cx > 0 && utf8_cont((unsigned char)l->s[cx])) cx--;
    return cx;
}
int utf8_next(Line *l, int cx) {
    if (cx >= l->len) return l->len;
    cx++;
    while (cx < l->len && utf8_cont((unsigned char)l->s[cx])) cx++;
    return cx;
}
static void word_left(Buf *b) {
    if (b->cx == 0) { if (b->cy > 0) { b->cy--; b->cx = b->ln[b->cy].len; } return; }
    Line *l = &b->ln[b->cy];
    int i = b->cx;
    while (i > 0 && !word_ch(l->s[i-1])) i--;
    while (i > 0 && word_ch(l->s[i-1])) i--;
    b->cx = i;
}
static void word_right(Buf *b) {
    Line *l = &b->ln[b->cy];
    if (b->cx >= l->len) { if (b->cy < b->n - 1) { b->cy++; b->cx = 0; } return; }
    int i = b->cx;
    while (i < l->len && !word_ch(l->s[i])) i++;
    while (i < l->len && word_ch(l->s[i])) i++;
    b->cx = i;
}
void move_cursor(Buf *b, int kind, int shift) {
    if (shift && !b->sel) { b->sel = 1; b->ay = b->cy; b->ax = b->cx; }
    if (!shift) b->sel = 0;
    Line *l = &b->ln[b->cy];
    int page = LINES - 3;
    /* with wrap on, Up/Down step one *visual* row, like VS Code */
    int tw = g_wtw;
    if (wrap && tw > 0 && (kind == M_UP || kind == M_DOWN)) {
        int rx = rx_of(l, b->cx);
        int lastseg = line_rows(b, b->cy, tw) - 1;
        if (kind == M_DOWN) {
            if (rx / tw < lastseg)                          /* stay, next segment */
                b->cx = cx_of_rx(l, rx + tw);
            else if (b->cy < b->n - 1) {
                b->cy++;
                b->cx = cx_of_rx(&b->ln[b->cy], rx % tw);
            }
        } else {
            if (rx >= tw) b->cx = cx_of_rx(l, rx - tw);
            else if (b->cy > 0) {
                b->cy--;
                Line *p = &b->ln[b->cy];
                int last = (line_rows(b, b->cy, tw) - 1) * tw;
                b->cx = cx_of_rx(p, last + rx % tw);
            }
        }
        if (b->cx > b->ln[b->cy].len) b->cx = b->ln[b->cy].len;
        g_lastkind = AK_OTHER;
        return;
    }
    switch (kind) {
        case M_UP:    if (b->cy > 0) b->cy--; break;
        case M_DOWN:  if (b->cy < b->n - 1) b->cy++; break;
        case M_LEFT:
            if (b->cx > 0) b->cx = utf8_prev(l, b->cx);
            else if (b->cy > 0) { b->cy--; b->cx = b->ln[b->cy].len; }
            break;
        case M_RIGHT:
            if (b->cx < l->len) b->cx = utf8_next(l, b->cx);
            else if (b->cy < b->n - 1) { b->cy++; b->cx = 0; }
            break;
        case M_HOME: {                        /* smart home */
            int fw = 0;
            while (fw < l->len && isspace((unsigned char)l->s[fw])) fw++;
            b->cx = (b->cx == fw) ? 0 : fw;
            break;
        }
        case M_END:   b->cx = l->len; break;
        case M_PGUP:  b->cy = max2(0, b->cy - page); break;
        case M_PGDN:  b->cy = min2(b->n - 1, b->cy + page); break;
        case M_WORDL: word_left(b); break;
        case M_WORDR: word_right(b); break;
        case M_DOCHOME: b->cy = 0; b->cx = 0; break;
        case M_DOCEND:  b->cy = b->n - 1; b->cx = b->ln[b->cy].len; break;
    }
    if (b->cx > b->ln[b->cy].len) b->cx = b->ln[b->cy].len;
    g_lastkind = AK_OTHER;
}
