/* editor.h — text buffer, undo and editing */
#ifndef SDS_EDITOR_H
#define SDS_EDITOR_H

#include "core/sds.h"

/* buffer.c */
void buf_insert_line(Buf *b, int at, const char *s, int len);
void buf_free(Buf *b);
Buf *buf_load(const char *path);
int buf_save(Buf *b);
int sel_norm(Buf *b, int *y1, int *x1, int *y2, int *x2);

/* undo.c */
enum { AK_OTHER, AK_TYPE, AK_BS };
void begin_action(int kind);
void edit_ins(Buf *b, int y, int x, const char *t, int len);
void edit_del(Buf *b, int y1, int x1, int y2, int x2);
void do_undo(Buf *b);
void do_redo(Buf *b);

/* clipboard.c */
extern char *clip;
extern int cliplen;

/* movement.c */
int utf8_cont(unsigned char c);

enum { M_UP, M_DOWN, M_LEFT, M_RIGHT, M_HOME, M_END, M_PGUP, M_PGDN,
       M_WORDL, M_WORDR, M_DOCHOME, M_DOCEND };

void move_cursor(Buf *b, int kind, int shift);

/* editing.c */
void ed_type(Buf *b, int c);
void ed_enter(Buf *b);
void ed_backspace(Buf *b);
void ed_delete(Buf *b);
void ed_tab(Buf *b, int dedent);
void ed_dup_line(Buf *b);
void ed_move_lines(Buf *b, int down);
void ed_toggle_comment(Buf *b);
void ed_copy(Buf *b, int cut);
void ed_paste_text(Buf *b, const char *t, int len);
void ed_open_below(Buf *b);
void handle_bracketed_paste(void);

/* complete.c */
void do_complete(void);

#endif /* SDS_EDITOR_H */
