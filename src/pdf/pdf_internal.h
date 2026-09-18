/* pdf_internal.h — PDF text extraction and page rendering (shared between the files in pdf/ only) */
#ifndef SDS_PDF_INTERNAL_H
#define SDS_PDF_INTERNAL_H

#include "core/sds.h"
#include "pdf/pdf.h"

/* parse.c */
PSpan pdf_dget(PSpan d, const char *key);
PSpan pdf_get(Pdf *pdf, PSpan s);
int pdf_arr_next(const char **p, const char *e, PSpan *out);
const char *pdf_arr_open(PSpan a);
char *pdf_stream(Pdf *pdf, PSpan d, size_t *outlen);
void pdf_index(Pdf *pdf);
void pdf_pages(Pdf *pdf);
void pdf_font_free(PdfFont *f);
void pdf_run(Pdf *pdf, const char *p, const char *e, PSpan res,
                    const double *ctm0, PdfOut *o, int depth);
void pdf_prepare(PdfOut *o);
void pdf_layout(PdfOut *o, Buf *b, int width);

/* raster.c */
int pdf_raster(const char *pdf, int page1, int W,
                      double pt_w, const char *out);
pid_t pdf_raster_start(const char *pdf, int page1, int W,
                       double pt_w, const char *out);
int pdf_raster_done(pid_t pid, const char *out, int wait);

#endif /* SDS_PDF_INTERNAL_H */
