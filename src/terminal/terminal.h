/* terminal.h — embedded terminal emulator */
#ifndef SDS_TERMINAL_H
#define SDS_TERMINAL_H

#include "core/sds.h"

/* emulator.c */
/* ── terminal emulator ────────────────────────────────────────────── */
/* A pty-backed shell living in a tab. The parser covers the subset of VT100 /
 * xterm that interactive shells and full-screen programs actually rely on:
 * cursor motion, erase, scroll regions, insert/delete, SGR colors, the
 * alternate screen and OSC titles. Explicitly not handled: mouse reporting,
 * sixel/graphics, double-width line attributes. */
#define TERM_SB_MAX 1000       /* scrollback lines kept per terminal */

typedef struct { char b[4]; unsigned char n, at; short fg, bg; } Cell;

enum { TA_BOLD = 1, TA_UNDER = 2, TA_REV = 4, TA_DIM = 8 };

struct Term {
    int    fd;
    pid_t  pid;
    int    rows, cols;
    Cell  *g;                  /* rows*cols, the visible screen */
    Cell  *alt;                /* alternate screen, allocated on first use */
    int    alt_on;
    int    cy, cx;
    int    scy, scx;           /* saved cursor (ESC 7 / CSI s) */
    unsigned char at; short fg, bg;
    int    top, bot;           /* scroll region, inclusive */
    int    wrapnext;           /* deferred wrap: cursor sits past the last col */
    int    hidecur;
    Cell  *sb;                 /* scrollback ring, TERM_SB_MAX rows of `cols` */
    int    sb_n, sb_head, sb_view;
    int    ps, np, params[8], priv, uexp;
    char   osc[128]; int oscn;
    int    dead, status;
    char   title[64];
    char   pend[4]; int pendn; /* partial UTF-8 arriving from the pty */
};

Cell *term_row(Term *t, int y);

/* terminal.c */
void term_size(Term *t, int rows, int cols);
void term_free(Term *t);
void term_key(Term *t, int c);
void open_terminal(void);
int any_live_term(void);
int pump_all_terms(void);

#endif /* SDS_TERMINAL_H */
