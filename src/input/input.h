/* input.h — key codes, key reading and mouse */
#ifndef SDS_INPUT_H
#define SDS_INPUT_H

#include "core/sds.h"

/* input.c */
/* ── key codes ────────────────────────────────────────────────────── */
/* Modified arrows/home/end arrive as CSI "1;<mod><dir>"; we register
 * every combination with define_key() so ncurses hands back one code.
 * mod: 2=Shift 3=Alt 4=Alt+Shift 5=Ctrl 6=Ctrl+Shift.
 * dir: 0=Up 1=Down 2=Left 3=Right 4=Home 5=End.                       */
#define MK(mod, dir) (2000 + (mod) * 10 + (dir))
enum { D_UP, D_DOWN, D_LEFT, D_RIGHT, D_HOME, D_END };
enum { K_PSTART = 2900, K_PEND, K_ADEL, K_AINS, K_ASENTER, K_MOUSE, K_NONE };
#define ALT(c)  (3000 + (c))

/* Alt+Shift+<digit>. A terminal cannot say "shift and the 2 key" — it sends
 * whatever that combination types, and which character that is depends on the
 * keyboard layout: Shift+2 is @ on a US board but " on a Swedish one. So the
 * shifted digit row is decoded through the active layout rather than assumed,
 * and these codes stand for the digit itself. */
#define PKEY(d) (3600 + (d))
#define IS_PKEY(c) ((c) >= PKEY(0) && (c) <= PKEY(9))
#undef  CTRL                      /* sys/ttydefaults.h (via pty.h) defines it */
#define CTRL(c) ((c) & 0x1f)

/* ── mouse ────────────────────────────────────────────────────────────
 * Reports arrive as CSI < btn ; col ; row M (press or drag) / m (release).
 * That SGR form is worth asking for over the original one, which cannot
 * name a column past 223 — on a wide terminal the right-hand pane would
 * simply be unclickable. read_key_raw() returns K_MOUSE and leaves the
 * event here, so the rest of the app keeps its one-int-per-key input. */
enum { MB_LEFT, MB_MID, MB_RIGHT, MB_NONE,
       MB_WHEEL_UP, MB_WHEEL_DOWN, MB_WHEEL_LEFT, MB_WHEEL_RIGHT };

typedef struct { int y, x, btn, press, motion, shift, alt, ctrl; } Mouse;
extern Mouse mev;
extern int mouse_cfg;
void mouse_enable(int on);
extern int g_timeout;
int read_key_raw(void);
int read_key(void);

/* keybindings.c */
/* ── keybindings ──────────────────────────────────────────────────── */
enum { KB_QUIT, KB_SAVE, KB_CLOSE_TAB, KB_HELP, KB_RUN, KB_TERM, KB_FIND,
       KB_FIND_NEXT, KB_REPLACE, KB_GOTO, KB_QUICKOPEN, KB_COMPLETE,
       KB_NEW_ENTRY, KB_DEL_ENTRY, KB_REFRESH, KB_TREE_UP, KB_TREE_DOWN,
       KB_TREE_COLLAPSE, KB_TREE_EXPAND, KB_TREE_OPEN, KB_TREE_OPEN_PANE,
       KB_TAB_PREV, KB_TAB_NEXT, KB_WRAP, KB_SIDEBAR, KB_MOVE_UP, KB_MOVE_DOWN,
       KB_PANE_LEFT, KB_PANE_RIGHT, KB_PANE_UP, KB_PANE_DOWN, KB_PANE_CLOSE,
       KB_MARKDOWN, KB_LINE_SCROLL, KB_N };

typedef struct { const char *name; int dflt; } KbDef;
extern const KbDef kb_def[];
extern int kb[KB_N];
extern char kb_row_cfg[64];
int parse_key(const char *s);

/* mouse.c */
void handle_mouse(void);

#endif /* SDS_INPUT_H */
