/* ui_internal.h — tabs, panes, drawing, themes and dialogs (shared between the files in ui/ only) */
#ifndef SDS_UI_INTERNAL_H
#define SDS_UI_INTERNAL_H

#include "core/sds.h"
#include "ui/ui.h"

/* theme.c */
enum { OV_SEL = 1, OV_FIND = 2, OV_BRK = 4 };

/* render.c */
attr_t md_attr(unsigned char st);
int draw_diags(Buf *b, int li, int scr_y, int scr_x, int tw, int maxrows);
int draw_row(Buf *b, int scr_y, int scr_x, int li, int tw, int maxrows,
                    int startseg);
void draw_tabbar(int w);
void draw_tree(int h);
void find_bracket(Buf *b);
void draw_term(Buf *b, int ytop, int x0, int rows, int cols);

/* panes.c */
Layout pane_layout(Rect a);
Rect editor_area(void);
extern int g_cy, g_cx;

#endif /* SDS_UI_INTERNAL_H */
