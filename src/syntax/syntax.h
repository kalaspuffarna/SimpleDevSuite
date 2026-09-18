/* syntax.h — languages, lexer and tree-sitter highlighting */
#ifndef SDS_SYNTAX_H
#define SDS_SYNTAX_H

#include "core/sds.h"

/* languages.c */
/* ── languages ────────────────────────────────────────────────────── */
enum { HA_DEF, HA_KW, HA_TYPE, HA_STR, HA_COM, HA_NUM, HA_PRE };
extern const Lang langs[];
/* Arrays elsewhere are sized by this, so it has to be a constant; languages.c
 * checks it against the table at compile time. */
#define NLANGS 19
#define LANG_TEXT (&langs[NLANGS - 1])
const Lang *lang_for(const char *path);

/* lexer.c */
void kw_index_build(void);
void ensure_hl(Buf *b, int upto);
void hl_line(Buf *b, int li, unsigned char *attr, int limit);

/* treesitter.c */
int ts_pending_ms(void);
#ifdef SDS_TREESITTER
void ts_offsets(Buf *b);
void ts_shift_spans(Buf *b, uint32_t sb, uint32_t ob, uint32_t nb);
void ts_forget(Buf *b);
void ts_collect(Buf *b, int y0, int y1);
#endif
int fetch_grammar(const char *lang);

#endif /* SDS_SYNTAX_H */
