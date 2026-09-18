/* pdf.h — PDF text extraction and page rendering */
#ifndef SDS_PDF_H
#define SDS_PDF_H

#include "core/sds.h"

/* parse.c */
typedef struct { const char *p, *e; } PSpan;

typedef struct { uint32_t code; char u[10]; } PdfUni;   /* code → UTF-8 */

typedef struct {
    int      twobyte;          /* codes are 2 bytes (Identity-H and friends) */
    PdfUni  *uni; int nuni;    /* sorted /ToUnicode map */
    double  *w; int firstchar, nw;                      /* simple-font widths */
    double   dw;                                        /* CID default width  */
    struct { uint32_t lo, hi; double w; } *cw; int ncw; /* CID /W ranges      */
} PdfFont;

typedef struct { PSpan dict, res; double mb[4]; } PdfPage;

/* `dir` is the quadrant the baseline advances in: 0 = +x, 1 = +y, 2 = -x,
 * 3 = -y. Layout rotates the page so the commonest one reads left to right,
 * which is what makes sideways scans and /Rotate'd pages legible. */
typedef struct { double x, y, h, w; char *s; int len; int dir; } PFrag;

typedef struct {
    PFrag  *f; int nf, fcap;
    int    *row; int nrow;         /* row k spans f[row[k] .. row[k+1]) */
    double  unit, left;            /* column width and page left edge, in pt */
    struct { const char *key; char nm[64]; PdfFont f; } fc[24];
    int nfc;
} PdfOut;

struct Pdf {
    char    *raw; size_t rawlen;
    PSpan   *obj; int nobj, objcap;
    char   **blob; int nblob;      /* decompressed /ObjStm bodies, kept alive */
    PdfPage *pg;  int npg;
    int      page;                 /* 0-based */
    int      encrypted;
    /* Extracting a page is the expensive half and does not depend on how wide
     * the pane is, so the fragments are kept and only re-flowed on a resize. */
    PdfOut   out;
    int      laid_w;               /* width b->ln was last laid out for */
    /* Page-image view. zoom is relative to "page width fills the pane", so a
     * half-width pane stays as readable as a full-width one and a resize
     * keeps the same apparent size. sx/sy scroll inside the rendered page,
     * and the image and view sizes below are what the last frame actually put
     * on screen, which is what the scroll keys work against. */
    double   zoom;
    int      sx, sy;               /* scroll into the page image, in pixels */
    int      img_w, img_h;         /* size of the page image last rendered */
    int      view_w, view_h;       /* pane size in pixels it was rendered for */
};

#define PDF_ZOOM_MIN 0.25
#define PDF_ZOOM_MAX 8.0
int pdf_utf8(uint32_t c, char *out);
double dabs(double v);

/* view.c */
extern int cfg_pdf_render;
extern double cfg_pdf_zoom;
void pdf_free(Pdf *p);
void pdf_relayout(Buf *b, int width);
void pdf_page_into(Buf *b, int page);

/* ── page-image view: scrolling and zoom ──────────────────────────────
 * All of these work in image pixels and only ever move the viewport; the
 * renderer clamps them against the page it actually produced. */
#define PDF_SCROLL_BOTTOM (1 << 28)     /* "as far down as this page goes" */
void pdf_scroll(Buf *b, int dy);
void pdf_pan(Buf *b, int dx);
void pdf_set_zoom(Buf *b, double z);
void pdf_fit_page(Buf *b);
Buf *pdf_load(const char *path);
int is_pdf_path(const char *path);

/* raster.c */
const char *pdf_render_why_not(void);
void cell_px(int *cw, int *ch);

/* kitty.c */
void gfx_reset_frame(void);
void gfx_request(int pane, Buf *b, int y, int x, int cols, int rows);
void gfx_flush(void);
void gfx_tick(void);
int  gfx_busy(void);
void gfx_clear_all(void);

#endif /* SDS_PDF_H */
