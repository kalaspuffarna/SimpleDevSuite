/* editor_internal.h — text buffer, undo and editing (shared between the files in editor/ only) */
#ifndef SDS_EDITOR_INTERNAL_H
#define SDS_EDITOR_INTERNAL_H

#include "core/sds.h"
#include "editor/editor.h"

/* buffer.c */
void urec_free(URec *r);
void hl_invalidate(Buf *b, int y);
void ins_text(Buf *b, int y, int x, const char *t, int len,
                     int *ey, int *ex);
char *range_text(Buf *b, int y1, int x1, int y2, int x2, int *outlen);
void del_range_raw(Buf *b, int y1, int x1, int y2, int x2);
void text_end(int y, int x, const char *t, int len, int *ey, int *ex);
void sel_delete(Buf *b);

/* undo.c */
extern int g_lastkind;

/* clipboard.c */
void clip_set(char *t, int len);

/* movement.c */
int utf8_prev(Line *l, int cx);
int utf8_next(Line *l, int cx);

#endif /* SDS_EDITOR_INTERNAL_H */
