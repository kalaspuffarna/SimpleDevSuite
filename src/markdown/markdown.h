/* markdown.h — markdown rendering */
#ifndef SDS_MARKDOWN_H
#define SDS_MARKDOWN_H

#include "core/sds.h"

/* markdown.c */
extern int cfg_md_preview;
extern int cfg_md_width;
enum { MS_BOLD = 1, MS_ITAL = 2, MS_UNDER = 4, MS_DIM = 8 };

struct Md {
    Line           *ln;        /* rendered lines */
    unsigned char **at;        /* style byte per byte of ln[i].s */
    int            *src;       /* buffer line each rendered line came from */
    int             n, cap;
    int             laid_w;    /* pane width this render was laid out for */
    int             ver;       /* b->ver it was built from */
    int             rowoff;    /* its own scroll position */
};

int md_cols(const char *s, int n);
int md_byte_at(const char *s, int n, int k);
void md_free(Md *m);
int md_run_of(const char *s, int len, int i, char c);
int md_is_hr(const char *s, int n);
int md_is_fence(const char *s, int n, char *ch, int *fl);
int md_atx(const char *s, int n, int *lvl, int *ts, int *tl);
int md_setext(const char *s, int n);
int md_bullet(const char *s, int n, int *mlen, int *ord);
void md_render(Buf *b, int width);
Md *md_view_of(Buf *b, int width);
int md_row_for_src(Md *m, int sy);
int md_is_md(Buf *b);

#endif /* SDS_MARKDOWN_H */
