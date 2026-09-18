#include "core/sds.h"
#include "pdf/pdf.h"
#include "pdf/pdf_internal.h"
#include "syntax/syntax.h"
#include "editor/editor.h"

/* config [pdf] */
int cfg_pdf_render = 1;          /* config [pdf] render */
double cfg_pdf_zoom = 1.0;       /* config [pdf] zoom, 100 = fit width */

/* ── page → buffer ────────────────────────────────────────────────── */
static void pdf_out_clear(PdfOut *o) {
    for (int i = 0; i < o->nf; i++) free(o->f[i].s);
    free(o->f);
    free(o->row);
    for (int i = 0; i < o->nfc; i++) pdf_font_free(&o->fc[i].f);
    memset(o, 0, sizeof *o);
}
void pdf_free(Pdf *p) {
    if (!p) return;
    pdf_out_clear(&p->out);
    for (int i = 0; i < p->nblob; i++) free(p->blob[i]);
    free(p->blob);
    free(p->obj);
    free(p->pg);
    free(p->raw);
    free(p);
}
/* Concatenate a page's /Contents (one stream, or an array of them). */
static char *pdf_contents(Pdf *pdf, PSpan page, size_t *outlen) {
    PSpan c = pdf_get(pdf, pdf_dget(page, "Contents"));
    char *all = NULL;
    size_t n = 0;
    const char *q = pdf_arr_open(c);
    PSpan v;
    for (int i = 0; ; i++) {
        PSpan item;
        if (q) {
            if (!pdf_arr_next(&q, c.e, &v)) break;
            item = pdf_get(pdf, v);
        } else {
            if (i > 0) break;
            item = c;
        }
        if (!item.p) continue;
        size_t sl;
        char *s = pdf_stream(pdf, item, &sl);
        if (!s) continue;
        char *t = realloc(all, n + sl + 2);
        if (!t) { free(s); break; }
        all = t;
        memcpy(all + n, s, sl);
        n += sl;
        all[n++] = '\n';                 /* streams split mid-token otherwise */
        free(s);
    }
    if (all) all[n] = 0;
    *outlen = n;
    return all;
}
/* Re-flow the cached page to `width` columns. Cheap: no re-parsing. */
void pdf_relayout(Buf *b, int width) {
    Pdf *pdf = b->pdf;
    for (int i = 0; i < b->n; i++) free(b->ln[i].s);
    b->n = 0;
    b->hl_upto = 0;
    b->ver++;
    pdf->laid_w = width;

    if (pdf->npg <= 0) {
        buf_insert_line(b, 0, "  no pages found in this PDF", 28);
        return;
    }
    pdf_layout(&pdf->out, b, width);
    if (b->n == 0) {
        const char *m = pdf->encrypted
            ? "  this PDF is encrypted — sds cannot extract its text"
            : "  no extractable text on this page (it may be a scanned image)";
        buf_insert_line(b, 0, m, (int)strlen(m));
    }
    if (b->n == 0) buf_insert_line(b, 0, "", 0);
    if (b->cy >= b->n) b->cy = b->n - 1;
    if (b->cy < 0) b->cy = 0;
    if (b->cx > b->ln[b->cy].len) b->cx = b->ln[b->cy].len;
    if (b->rowoff > b->n - 1) b->rowoff = max2(0, b->n - 1);
    b->coloff = 0;
    b->sel = 0;
}
/* Extract `page` and lay it out. The extraction is the slow half, so it is
 * cached in pdf->out and reused by pdf_relayout() on every resize. */
void pdf_page_into(Buf *b, int page) {
    Pdf *pdf = b->pdf;
    b->cy = b->cx = b->rowoff = b->coloff = b->subrow = 0;
    /* A new page starts at its top, and at the zoom the config asks for:
     * carrying a zoom across pages leaves you somewhere in the middle of the
     * next one, and re-renders it at a size nothing else is cached at. */
    pdf->sx = pdf->sy = 0;
    pdf->zoom = cfg_pdf_zoom;
    if (page < 0) page = 0;
    if (page >= pdf->npg) page = pdf->npg - 1;
    pdf->page = page;

    pdf_out_clear(&pdf->out);
    if (pdf->npg > 0) {
        PdfPage *g = &pdf->pg[page];
        size_t n;
        char *body = pdf_contents(pdf, g->dict, &n);
        if (body) {
            double ctm[6] = { 1, 0, 0, 1, 0, 0 };
            pdf_run(pdf, body, body + n, g->res, ctm, &pdf->out, 0);
            free(body);
        }
        pdf_prepare(&pdf->out);
    }
    pdf_relayout(b, pdf->laid_w);
}

/* Scroll by dy pixels, rolling onto the next or previous page at the ends —
 * reading a document straight through shouldn't need the page keys too. */
void pdf_scroll(Buf *b, int dy) {
    Pdf *p = b->pdf;
    int maxy = max2(0, p->img_h - p->view_h);
    int ny = p->sy + dy;
    if (ny > maxy) {
        if (p->sy < maxy) { p->sy = maxy; return; }
        if (p->page + 1 >= p->npg) { set_msg("last page%s", ""); return; }
        pdf_page_into(b, p->page + 1);
    } else if (ny < 0) {
        if (p->sy > 0) { p->sy = 0; return; }
        if (p->page <= 0) { set_msg("first page%s", ""); return; }
        pdf_page_into(b, p->page - 1);
        p->sy = PDF_SCROLL_BOTTOM;
    } else {
        p->sy = ny;
    }
}
void pdf_pan(Buf *b, int dx) {
    Pdf *p = b->pdf;
    p->sx = max2(0, min2(p->sx + dx, max2(0, p->img_w - p->view_w)));
}
/* Zoom about the middle of the viewport, so whatever is being read stays on
 * screen instead of sliding out of it. */
void pdf_set_zoom(Buf *b, double z) {
    Pdf *p = b->pdf;
    if (z < PDF_ZOOM_MIN) z = PDF_ZOOM_MIN;
    if (z > PDF_ZOOM_MAX) z = PDF_ZOOM_MAX;
    if (z == p->zoom) return;
    double s = z / p->zoom;
    p->zoom = z;
    if (p->img_w > 0) {
        p->sx = max2(0, (int)((p->sx + p->view_w / 2.0) * s - p->view_w / 2.0));
        p->sy = max2(0, (int)((p->sy + p->view_h / 2.0) * s - p->view_h / 2.0));
    }
    char pct[32];
    snprintf(pct, sizeof pct, "%d%%", (int)(z * 100 + 0.5));
    set_msg("zoom %s", pct);
}
/* Shrink until the whole page is inside the pane; the reverse of the default,
 * which fills the pane's width and scrolls. */
void pdf_fit_page(Buf *b) {
    Pdf *p = b->pdf;
    if (p->img_w <= 0 || p->img_h <= 0) return;
    double s = (double)p->view_w / p->img_w;
    if ((double)p->view_h / p->img_h < s) s = (double)p->view_h / p->img_h;
    p->sx = p->sy = 0;
    pdf_set_zoom(b, p->zoom * s);
}
Buf *pdf_load(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0 || sz > (400L << 20)) { fclose(f); return NULL; }
    rewind(f);
    char *raw = malloc((size_t)sz + 1);
    if (!raw) { fclose(f); return NULL; }
    size_t got = fread(raw, 1, (size_t)sz, f);
    fclose(f);
    raw[got] = 0;

    Pdf *pdf = calloc(1, sizeof *pdf);
    if (!pdf) { free(raw); return NULL; }
    pdf->raw = raw;
    pdf->rawlen = got;
    pdf->zoom = cfg_pdf_zoom;          /* 1.0 = page width fills the pane */
    for (size_t i = 0; i + 8 <= got; i++)
        if (raw[i] == '/' && !memcmp(raw + i, "/Encrypt", 8)) { pdf->encrypted = 1; break; }
    pdf_index(pdf);
    pdf_pages(pdf);

    Buf *b = calloc(1, sizeof *b);
    if (!b) { pdf_free(pdf); return NULL; }
    snprintf(b->path, sizeof b->path, "%s", path);
    const char *slash = strrchr(path, '/');
    snprintf(b->name, sizeof b->name, "%s", slash ? slash + 1 : path);
    b->lang = LANG_TEXT;
    b->kind = TAB_PDF;
    b->pdf = pdf;
    /* Show the real page when the terminal and a rasterizer allow it; say why
     * when they do not, rather than silently dropping to text. */
    const char *why = pdf_render_why_not();
    b->pdf_img = (why == NULL);
    if (why) set_msg("%s", why);
    pdf_page_into(b, 0);
    return b;
}

int is_pdf_path(const char *path) {
    const char *dot = strrchr(path, '.');
    return dot && !strcasecmp(dot, ".pdf");
}
