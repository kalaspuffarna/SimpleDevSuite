#include "core/sds.h"

/* Runtime-configurable (see ~/.config/sds/config). Both were compile-time
 * constants before the config system; they are still fixed after startup, so
 * buffers sized against them only need to allow for TABSTOP_MAX. */
int tabstop = 4;            /* render width of a tab character */

char  msg[PATH_MAX + 64] = "";
int   pending_close = 0, pending_quit = 0, show_help = 0;
int   wrap = 0;                 /* soft-wrap long lines (Alt+Z) */
/* Without wrap, a line longer than the pane scrolls sideways. With this on
 * only the cursor's line does, and every other line stays at its start, so
 * one long line doesn't push the rest of the file out of view (Alt+L). */
int   line_scroll = 1;
int   g_wtw = 1;                /* text width in use, set by draw_editor */

/* Tree-sitter parses a file as a whole: the first parse of a megabyte of C
 * takes over a second, and every edit after it costs time proportional to the
 * file, not to the edit. Past this size the built-in lexer takes over, which
 * is per-line work. 0 removes the limit. */
int   cfg_ts_max_kb = 512;
