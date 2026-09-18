/* syntax_internal.h — languages, lexer and tree-sitter highlighting (shared between the files in syntax/ only) */
#ifndef SDS_SYNTAX_INTERNAL_H
#define SDS_SYNTAX_INTERNAL_H

#include "core/sds.h"
#include "syntax/syntax.h"

/* languages.c */
/* lexer states carried across lines */
enum { ST_NORM = 0, ST_BCOM, ST_TRI1, ST_TRI2, ST_MDFENCE };

/* treesitter.c */
#ifdef SDS_TREESITTER
int ts_line_attrs(Buf *b, int li, unsigned char *attr, int limit);
#endif

#endif /* SDS_SYNTAX_INTERNAL_H */
