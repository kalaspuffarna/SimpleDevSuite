#include "core/sds.h"
#include "pdf/pdf.h"
#include "pdf/pdf_internal.h"
#include "editor/editor.h"
#include "ui/ui.h"
#include "tree/tree.h"

/* ── PDF ──────────────────────────────────────────────────────────────
 * A read-only PDF text extractor: enough to actually read a document in a
 * terminal, not a renderer. It indexes objects, decodes streams, walks the
 * page tree, and interprets the text-showing operators of a content stream.
 *
 * Two things make the output legible rather than a pile of letters. Glyph
 * codes go through the font's /ToUnicode map when it has one, which is what
 * turns a subset font back into words. And every run of text keeps the device
 * position it was drawn at, so the layout pass can rebuild rows and columns —
 * indented code and side-by-side columns survive the trip.
 *
 * The object index is built by scanning the whole file for "N G obj" rather
 * than by reading the xref. That costs one pass but sidesteps xref tables,
 * xref streams, incremental updates and damaged files all at once; objects
 * packed into /ObjStm are unpacked afterwards. Not handled: encryption, LZW,
 * images, and vertical writing. A page that yields no text says so.        */

#define PDF_MAXOBJ  400000     /* object-number ceiling, guards a bad parse */
#define PDF_MAXPAGE  20000
#define PDF_MAXFRAG 200000     /* text runs kept for one page */
#define PDF_MAXCOL    4000     /* widest line the layout will emit */

/* ── lexing ───────────────────────────────────────────────────────── */
static int pdf_ws(int c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == 0;
}
static int pdf_delim(int c) {
    return c == '(' || c == ')' || c == '<' || c == '>' || c == '[' ||
           c == ']' || c == '{' || c == '}' || c == '/' || c == '%';
}
static const char *pdf_skip_ws(const char *p, const char *e) {
    for (;;) {
        while (p < e && pdf_ws((unsigned char)*p)) p++;
        if (p < e && *p == '%') {
            while (p < e && *p != '\n' && *p != '\r') p++;
            continue;
        }
        return p;
    }
}
/* Advance past exactly one object: dict, array, string, name, number or an
 * "N G R" reference (which has to be swallowed whole or the G would read as
 * the next value). */
static const char *pdf_skip_obj(const char *p, const char *e) {
    p = pdf_skip_ws(p, e);
    if (p >= e) return e;
    if (*p == '<' && p + 1 < e && p[1] == '<') {
        p += 2;
        for (;;) {
            p = pdf_skip_ws(p, e);
            if (p >= e) return e;
            if (*p == '>') return (p + 1 < e && p[1] == '>') ? p + 2 : p + 1;
            const char *q = pdf_skip_obj(p, e);
            if (q <= p) return e;
            p = q;
        }
    }
    if (*p == '[' || *p == '{') {
        char close = (*p == '[') ? ']' : '}';
        p++;
        for (;;) {
            p = pdf_skip_ws(p, e);
            if (p >= e) return e;
            if (*p == close) return p + 1;
            const char *q = pdf_skip_obj(p, e);
            if (q <= p) return e;
            p = q;
        }
    }
    if (*p == '(') {
        int d = 1;
        p++;
        while (p < e && d) {
            if (*p == '\\') { p += 2; continue; }
            if (*p == '(') d++;
            else if (*p == ')' && --d == 0) { p++; break; }
            p++;
        }
        return p;
    }
    if (*p == '<') { while (p < e && *p != '>') p++; return p < e ? p + 1 : e; }
    if (*p == '/') {
        p++;
        while (p < e && !pdf_ws((unsigned char)*p) && !pdf_delim((unsigned char)*p)) p++;
        return p;
    }
    if (*p == ']' || *p == '>' || *p == ')' || *p == '}') return p + 1;  /* stray */
    if (isdigit((unsigned char)*p) || *p == '+' || *p == '-' || *p == '.') {
        const char *q = p;
        while (q < e && (isdigit((unsigned char)*q) || *q == '+' || *q == '-' ||
                         *q == '.' || *q == 'e' || *q == 'E')) q++;
        const char *r = pdf_skip_ws(q, e);              /* "N G R"? */
        if (r < e && isdigit((unsigned char)*r)) {
            const char *s = r;
            while (s < e && isdigit((unsigned char)*s)) s++;
            const char *t = pdf_skip_ws(s, e);
            if (t < e && *t == 'R' &&
                (t + 1 >= e || pdf_ws((unsigned char)t[1]) || pdf_delim((unsigned char)t[1])))
                return t + 1;
        }
        return q;
    }
    {                                        /* bare keyword: true/false/null */
        const char *q = p;
        while (q < e && !pdf_ws((unsigned char)*q) && !pdf_delim((unsigned char)*q)) q++;
        return q > p ? q : p + 1;
    }
}
/* Read a /Name into `out`, decoding #xx escapes. NULL if there isn't one. */
static const char *pdf_name(const char *p, const char *e, char *out, size_t cap) {
    p = pdf_skip_ws(p, e);
    if (p >= e || *p != '/') return NULL;
    p++;
    size_t n = 0;
    while (p < e && !pdf_ws((unsigned char)*p) && !pdf_delim((unsigned char)*p)) {
        int c = (unsigned char)*p++;
        if (c == '#' && p + 1 < e && isxdigit((unsigned char)p[0]) &&
            isxdigit((unsigned char)p[1])) {
            char h[3] = { p[0], p[1], 0 };
            c = (int)strtol(h, NULL, 16);
            p += 2;
        }
        if (n + 1 < cap) out[n++] = (char)c;
    }
    out[n] = 0;
    return p;
}
static int pdf_isname(PSpan s, const char *want) {
    char nm[64];
    if (!s.p || !pdf_name(s.p, s.e, nm, sizeof nm)) return 0;
    return !strcmp(nm, want);
}
/* Value of /key in the dict starting at d.p, unresolved. */
PSpan pdf_dget(PSpan d, const char *key) {
    PSpan none = { NULL, NULL };
    if (!d.p) return none;
    const char *p = pdf_skip_ws(d.p, d.e);
    if (p + 1 >= d.e || p[0] != '<' || p[1] != '<') return none;
    p += 2;
    for (;;) {
        p = pdf_skip_ws(p, d.e);
        if (p >= d.e || *p == '>') return none;
        char nm[64];
        const char *q = pdf_name(p, d.e, nm, sizeof nm);
        if (!q) return none;                              /* not a key: give up */
        const char *v = pdf_skip_ws(q, d.e);
        const char *ve = pdf_skip_obj(v, d.e);
        if (ve <= v) return none;
        if (!strcmp(nm, key)) { PSpan s = { v, ve }; return s; }
        p = ve;
    }
}
static int pdf_isref(PSpan s, int *num) {
    if (!s.p) return 0;
    const char *p = pdf_skip_ws(s.p, s.e);
    if (p >= s.e || !isdigit((unsigned char)*p)) return 0;
    char *q;
    long n = strtol(p, &q, 10);
    p = pdf_skip_ws(q, s.e);
    if (p >= s.e || !isdigit((unsigned char)*p)) return 0;
    strtol(p, &q, 10);
    p = pdf_skip_ws(q, s.e);
    if (p >= s.e || *p != 'R') return 0;
    *num = (int)n;
    return 1;
}
PSpan pdf_get(Pdf *pdf, PSpan s) {          /* follow indirect refs */
    for (int i = 0; i < 32; i++) {
        int n;
        if (!pdf_isref(s, &n)) return s;
        if (n < 0 || n >= pdf->nobj || !pdf->obj[n].p) {
            PSpan none = { NULL, NULL };
            return none;
        }
        s = pdf->obj[n];
    }
    return s;
}
static PSpan pdf_dgetr(Pdf *pdf, PSpan d, const char *key) {
    return pdf_get(pdf, pdf_dget(d, key));
}
static double pdf_num(PSpan s, double dflt) {
    if (!s.p) return dflt;
    const char *p = pdf_skip_ws(s.p, s.e);
    if (p >= s.e) return dflt;
    if (!isdigit((unsigned char)*p) && *p != '-' && *p != '+' && *p != '.') return dflt;
    return atof(p);
}
static double pdf_dnum(Pdf *pdf, PSpan d, const char *key, double dflt) {
    return pdf_num(pdf_dgetr(pdf, d, key), dflt);
}
/* Iterate an array: on entry *p is inside the array, returns each element. */
int pdf_arr_next(const char **p, const char *e, PSpan *out) {
    const char *s = pdf_skip_ws(*p, e);
    if (s >= e || *s == ']') return 0;
    const char *q = pdf_skip_obj(s, e);
    if (q <= s) return 0;
    out->p = s; out->e = q;
    *p = q;
    return 1;
}
const char *pdf_arr_open(PSpan a) {   /* NULL when a isn't an array */
    if (!a.p) return NULL;
    const char *p = pdf_skip_ws(a.p, a.e);
    return (p < a.e && *p == '[') ? p + 1 : NULL;
}

/* ── stream filters ───────────────────────────────────────────────── */
static char *pdf_inflate(const char *in, size_t inlen, size_t *outlen) {
    z_stream z;
    memset(&z, 0, sizeof z);
    if (inflateInit(&z) != Z_OK) return NULL;
    size_t cap = inlen * 4 + 4096, n = 0;
    char *out = malloc(cap);
    if (!out) { inflateEnd(&z); return NULL; }
    z.next_in = (Bytef *)in;
    z.avail_in = (uInt)inlen;
    for (;;) {
        if (n == cap) {
            if (cap > ((size_t)256 << 20)) break;            /* runaway guard */
            char *t = realloc(out, cap * 2);
            if (!t) break;
            out = t; cap *= 2;
        }
        z.next_out = (Bytef *)out + n;
        z.avail_out = (uInt)(cap - n);
        int r = inflate(&z, Z_NO_FLUSH);
        n = cap - z.avail_out;
        if (r != Z_OK) break;                     /* end, or truncated: keep it */
        if (z.avail_in == 0 && z.avail_out != 0) break;
    }
    inflateEnd(&z);
    char *t = realloc(out, n + 1);
    if (!t) { free(out); return NULL; }
    t[n] = 0;
    *outlen = n;
    return t;
}
static char *pdf_a85(const char *in, size_t inlen, size_t *outlen) {
    char *out = malloc(inlen * 4 / 5 + 8);
    if (!out) return NULL;
    size_t n = 0;
    uint32_t acc = 0;
    int k = 0;
    for (size_t i = 0; i < inlen; i++) {
        int c = (unsigned char)in[i];
        if (pdf_ws(c)) continue;
        if (c == '~') break;
        if (c == 'z' && k == 0) { for (int j = 0; j < 4; j++) out[n++] = 0; continue; }
        if (c < '!' || c > 'u') continue;
        acc = acc * 85 + (uint32_t)(c - '!');
        if (++k == 5) {
            for (int j = 3; j >= 0; j--) out[n + j] = (char)(acc >> (8 * (3 - j)));
            n += 4; acc = 0; k = 0;
        }
    }
    if (k > 1) {                                   /* partial group, zero-padded */
        for (int j = k; j < 5; j++) acc = acc * 85 + 84;
        for (int j = 0; j < k - 1; j++) out[n++] = (char)(acc >> (8 * (3 - j)));
    }
    out[n] = 0;
    *outlen = n;
    return out;
}
static char *pdf_ahx(const char *in, size_t inlen, size_t *outlen) {
    char *out = malloc(inlen / 2 + 2);
    if (!out) return NULL;
    size_t n = 0;
    int hi = -1;
    for (size_t i = 0; i < inlen; i++) {
        int c = (unsigned char)in[i];
        if (c == '>') break;
        if (!isxdigit(c)) continue;
        int v = isdigit(c) ? c - '0' : (tolower(c) - 'a' + 10);
        if (hi < 0) hi = v;
        else { out[n++] = (char)((hi << 4) | v); hi = -1; }
    }
    if (hi >= 0) out[n++] = (char)(hi << 4);
    out[n] = 0;
    *outlen = n;
    return out;
}
/* PNG / TIFF predictors, as used by /ObjStm and xref streams. */
static void pdf_unpredict(char **buf, size_t *len, int pred, int colors,
                          int bpc, int columns) {
    if (pred < 2 || colors < 1 || bpc < 1 || columns < 1) return;
    int bpp = (colors * bpc + 7) / 8;
    size_t rowlen = ((size_t)columns * colors * bpc + 7) / 8;
    if (bpp < 1) bpp = 1;
    unsigned char *d = (unsigned char *)*buf;
    if (pred == 2) {                                       /* TIFF, 8bpc only */
        if (bpc != 8) return;
        for (size_t r = 0; r + rowlen <= *len; r += rowlen)
            for (size_t i = (size_t)bpp; i < rowlen; i++)
                d[r + i] = (unsigned char)(d[r + i] + d[r + i - bpp]);
        return;
    }
    size_t nrow = *len / (rowlen + 1), o = 0;
    unsigned char *out = malloc(nrow * rowlen + 1);
    if (!out) return;
    for (size_t r = 0; r < nrow; r++) {
        unsigned char ft = d[r * (rowlen + 1)];
        const unsigned char *src = d + r * (rowlen + 1) + 1;
        unsigned char *cur = out + o, *up = (r ? cur - rowlen : NULL);
        for (size_t i = 0; i < rowlen; i++) {
            int a = (i >= (size_t)bpp) ? cur[i - bpp] : 0;
            int b = up ? up[i] : 0;
            int c = (up && i >= (size_t)bpp) ? up[i - bpp] : 0;
            int x = src[i];
            switch (ft) {
                case 1: x += a; break;
                case 2: x += b; break;
                case 3: x += (a + b) / 2; break;
                case 4: {
                    int p = a + b - c;
                    int pa = p > a ? p - a : a - p;
                    int pb = p > b ? p - b : b - p;
                    int pc = p > c ? p - c : c - p;
                    x += (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
                    break;
                }
                default: break;
            }
            cur[i] = (unsigned char)x;
        }
        o += rowlen;
    }
    out[o] = 0;
    free(*buf);
    *buf = (char *)out;
    *len = o;
}
/* Decode the stream attached to the object whose body is `d`. */
char *pdf_stream(Pdf *pdf, PSpan d, size_t *outlen) {
    *outlen = 0;
    const char *p = pdf_skip_ws(d.p, d.e);
    if (p + 1 >= d.e || p[0] != '<' || p[1] != '<') return NULL;
    const char *dend = pdf_skip_obj(p, d.e);
    PSpan dict = { p, dend };
    p = pdf_skip_ws(dend, d.e);
    if (d.e - p < 6 || memcmp(p, "stream", 6) != 0) return NULL;
    p += 6;
    if (p < d.e && *p == '\r') p++;
    if (p < d.e && *p == '\n') p++;

    long n = (long)pdf_dnum(pdf, dict, "Length", -1);
    if (n < 0 || p + n > d.e) {                  /* bad or missing /Length */
        const char *stop = NULL;
        for (const char *q = p; q + 9 <= d.e; q++)
            if (*q == 'e' && memcmp(q, "endstream", 9) == 0) { stop = q; break; }
        n = stop ? (long)(stop - p) : (long)(d.e - p);
        while (n > 0 && (p[n - 1] == '\n' || p[n - 1] == '\r')) n--;
    }
    if (n < 0) return NULL;
    char *data = malloc((size_t)n + 1);
    if (!data) return NULL;
    memcpy(data, p, (size_t)n);
    data[n] = 0;
    size_t dl = (size_t)n;

    PSpan f = pdf_dgetr(pdf, dict, "Filter");
    const char *fp = f.p, *fe = f.e;
    int arr = 0;
    if (fp) {
        const char *s = pdf_skip_ws(fp, fe);
        if (s < fe && *s == '[') { arr = 1; fp = s + 1; }
    }
    for (int k = 0; k < 8 && fp && fp < fe; k++) {
        char nm[64];
        const char *q = pdf_name(fp, fe, nm, sizeof nm);
        if (!q) break;
        fp = q;
        char *nd = NULL;
        size_t nl = 0;
        if (!strcmp(nm, "FlateDecode") || !strcmp(nm, "Fl"))
            nd = pdf_inflate(data, dl, &nl);
        else if (!strcmp(nm, "ASCII85Decode") || !strcmp(nm, "A85"))
            nd = pdf_a85(data, dl, &nl);
        else if (!strcmp(nm, "ASCIIHexDecode") || !strcmp(nm, "AHx"))
            nd = pdf_ahx(data, dl, &nl);
        else break;                       /* LZW, DCT, … : hand back what we have */
        free(data);
        if (!nd) return NULL;
        data = nd; dl = nl;
        if (!arr) break;
    }
    PSpan parms = pdf_dgetr(pdf, dict, "DecodeParms");
    int pred = (int)pdf_dnum(pdf, parms, "Predictor", 1);
    if (pred > 1)
        pdf_unpredict(&data, &dl, pred,
                      (int)pdf_dnum(pdf, parms, "Colors", 1),
                      (int)pdf_dnum(pdf, parms, "BitsPerComponent", 8),
                      (int)pdf_dnum(pdf, parms, "Columns", 1));
    *outlen = dl;
    return data;
}

/* ── object index ─────────────────────────────────────────────────── */
static void pdf_obj_room(Pdf *pdf, int num) {
    if (num < pdf->nobj) return;
    int want = num + 1;
    if (want > pdf->objcap) {          /* doubling, not one copy per object */
        pdf->objcap = pdf->objcap * 2 > want ? pdf->objcap * 2 : want;
        pdf->obj = xrealloc(pdf->obj, (size_t)pdf->objcap * sizeof *pdf->obj);
    }
    memset(pdf->obj + pdf->nobj, 0, (size_t)(want - pdf->nobj) * sizeof *pdf->obj);
    pdf->nobj = want;
}
void pdf_index(Pdf *pdf) {
    const char *b = pdf->raw, *e = b + pdf->rawlen;
    int *num = NULL;
    const char **hdr = NULL, **body = NULL;
    int n = 0, cap = 0;

    /* Every "<num> <gen> obj" in the file, in file order. */
    for (const char *p = b; p + 3 <= e; p++) {
        if (p[0] != 'o' || p[1] != 'b' || p[2] != 'j') continue;
        if (p + 3 < e && !pdf_ws((unsigned char)p[3]) && !pdf_delim((unsigned char)p[3]))
            continue;
        const char *q = p;
        while (q > b && pdf_ws((unsigned char)q[-1])) q--;
        if (q == p) continue;                        /* "obj" needs space before */
        const char *ge = q;
        while (q > b && isdigit((unsigned char)q[-1])) q--;
        if (q == ge) continue;
        const char *gs = q;
        while (q > b && pdf_ws((unsigned char)q[-1])) q--;
        if (q == gs) continue;
        const char *ne = q;
        while (q > b && isdigit((unsigned char)q[-1])) q--;
        if (q == ne) continue;
        if (q > b && !pdf_ws((unsigned char)q[-1]) && !pdf_delim((unsigned char)q[-1]))
            continue;
        long id = strtol(q, NULL, 10);
        if (id < 0 || id > PDF_MAXOBJ) continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 256;
            num  = xrealloc(num,  (size_t)cap * sizeof *num);
            hdr  = xrealloc(hdr,  (size_t)cap * sizeof *hdr);
            body = xrealloc(body, (size_t)cap * sizeof *body);
        }
        num[n] = (int)id;
        hdr[n] = q;
        body[n] = p + 3;
        n++;
        p += 2;
    }
    /* Each object runs to its "endobj", or to the next header if it has none.
     * Later definitions win, which is what an incremental update wants. */
    for (int i = 0; i < n; i++) {
        const char *limit = (i + 1 < n) ? hdr[i + 1] : e;
        const char *end = limit;
        for (const char *q = body[i]; q + 6 <= limit; q++)
            if (*q == 'e' && memcmp(q, "endobj", 6) == 0) { end = q; break; }
        pdf_obj_room(pdf, num[i]);
        pdf->obj[num[i]].p = body[i];
        pdf->obj[num[i]].e = end;
    }
    free(num); free(hdr); free(body);

    /* Unpack /ObjStm containers. Objects already defined at the top level win,
     * so a hybrid file's newer definitions are not clobbered. */
    for (int i = 0; i < pdf->nobj; i++) {
        PSpan d = pdf->obj[i];
        if (!d.p || !pdf_isname(pdf_dget(d, "Type"), "ObjStm")) continue;
        size_t dl;
        char *data = pdf_stream(pdf, d, &dl);
        if (!data) continue;
        int cnt = (int)pdf_dnum(pdf, d, "N", 0);
        long first = (long)pdf_dnum(pdf, d, "First", 0);
        if (cnt <= 0 || cnt > PDF_MAXOBJ || first <= 0 || (size_t)first > dl) {
            free(data);
            continue;
        }
        pdf->blob = xrealloc(pdf->blob, (size_t)(pdf->nblob + 1) * sizeof *pdf->blob);
        pdf->blob[pdf->nblob++] = data;

        int *on = xmalloc((size_t)cnt * sizeof *on);
        long *oo = xmalloc((size_t)cnt * sizeof *oo);
        const char *hp = data, *he = data + first;
        int got = 0;
        for (; got < cnt; got++) {
            char *q;
            hp = pdf_skip_ws(hp, he);
            if (hp >= he || !isdigit((unsigned char)*hp)) break;
            on[got] = (int)strtol(hp, &q, 10);
            hp = pdf_skip_ws(q, he);
            if (hp >= he || !isdigit((unsigned char)*hp)) break;
            oo[got] = strtol(hp, &q, 10);
            hp = q;
        }
        for (int k = 0; k < got; k++) {
            long s = first + oo[k];
            long en = (k + 1 < got) ? first + oo[k + 1] : (long)dl;
            if (on[k] < 0 || on[k] > PDF_MAXOBJ || s < 0 || en > (long)dl || en < s)
                continue;
            pdf_obj_room(pdf, on[k]);
            if (pdf->obj[on[k]].p) continue;
            pdf->obj[on[k]].p = data + s;
            pdf->obj[on[k]].e = data + en;
        }
        free(on); free(oo);
    }
}

/* ── page tree ────────────────────────────────────────────────────── */
static void pdf_rect(PSpan a, double *out) {
    const char *p = pdf_arr_open(a);
    if (!p) return;
    PSpan v;
    for (int i = 0; i < 4 && pdf_arr_next(&p, a.e, &v); i++) out[i] = pdf_num(v, out[i]);
    if (out[2] < out[0]) { double t = out[0]; out[0] = out[2]; out[2] = t; }
    if (out[3] < out[1]) { double t = out[1]; out[1] = out[3]; out[3] = t; }
}
static void pdf_addpage(Pdf *pdf, PSpan dict, PSpan res, const double *mb) {
    if (pdf->npg >= PDF_MAXPAGE) return;
    if ((pdf->npg & 63) == 0)
        pdf->pg = xrealloc(pdf->pg, (size_t)(pdf->npg + 64) * sizeof *pdf->pg);
    PdfPage *g = &pdf->pg[pdf->npg++];
    g->dict = dict;
    g->res = res;
    memcpy(g->mb, mb, 4 * sizeof(double));
}
static void pdf_walk(Pdf *pdf, PSpan node, PSpan res, const double *mb,
                     int depth) {
    if (depth > 48 || pdf->npg >= PDF_MAXPAGE || !node.p) return;
    PSpan r = pdf_dgetr(pdf, node, "Resources");
    if (r.p) res = r;
    double box[4] = { mb[0], mb[1], mb[2], mb[3] };
    PSpan m = pdf_dgetr(pdf, node, "MediaBox");
    if (m.p) pdf_rect(m, box);
    PSpan kids = pdf_dgetr(pdf, node, "Kids");
    const char *p = pdf_arr_open(kids);
    if (p) {
        PSpan k;
        while (pdf_arr_next(&p, kids.e, &k))
            pdf_walk(pdf, pdf_get(pdf, k), res, box, depth + 1);
        return;
    }
    pdf_addpage(pdf, node, res, box);
}
void pdf_pages(Pdf *pdf) {
    const char *b = pdf->raw, *e = b + pdf->rawlen;
    PSpan root = { NULL, NULL };
    for (const char *p = e - 7; p >= b && !root.p; p--) {     /* newest trailer */
        if (*p != 't' || memcmp(p, "trailer", 7) != 0) continue;
        PSpan t = { p + 7, e };
        PSpan r = pdf_dget(t, "Root");
        if (r.p) root = pdf_get(pdf, r);
    }
    if (!root.p)                              /* xref-stream file, or damaged */
        for (int i = 0; i < pdf->nobj; i++) {
            if (!pdf->obj[i].p) continue;
            if (pdf_isname(pdf_dget(pdf->obj[i], "Type"), "Catalog")) {
                root = pdf->obj[i];
                break;
            }
            PSpan r = pdf_dget(pdf->obj[i], "Root");
            if (r.p && pdf_isname(pdf_dget(pdf->obj[i], "Type"), "XRef")) {
                root = pdf_get(pdf, r);
                if (root.p) break;
            }
        }
    double mb[4] = { 0, 0, 612, 792 };
    PSpan none = { NULL, NULL };
    PSpan pages = pdf_dgetr(pdf, root, "Pages");
    if (pages.p) pdf_walk(pdf, pages, none, mb, 0);
    if (pdf->npg == 0)                        /* no usable tree: sweep for leaves */
        for (int i = 0; i < pdf->nobj; i++)
            if (pdf->obj[i].p && pdf_isname(pdf_dget(pdf->obj[i], "Type"), "Page"))
                pdf_addpage(pdf, pdf->obj[i],
                            pdf_dgetr(pdf, pdf->obj[i], "Resources"), mb);
}

/* ── fonts ────────────────────────────────────────────────────────── */
int pdf_utf8(uint32_t c, char *out) {
    if (c < 0x80)     { out[0] = (char)c; return 1; }
    if (c < 0x800)    { out[0] = (char)(0xc0 | (c >> 6));
                        out[1] = (char)(0x80 | (c & 0x3f)); return 2; }
    if (c < 0x10000)  { out[0] = (char)(0xe0 | (c >> 12));
                        out[1] = (char)(0x80 | ((c >> 6) & 0x3f));
                        out[2] = (char)(0x80 | (c & 0x3f)); return 3; }
    out[0] = (char)(0xf0 | (c >> 18));
    out[1] = (char)(0x80 | ((c >> 12) & 0x3f));
    out[2] = (char)(0x80 | ((c >> 6) & 0x3f));
    out[3] = (char)(0x80 | (c & 0x3f));
    return 4;
}
/* A <hex string> into bytes. Returns the byte count, -1 if it isn't one. */
static int pdf_hexstr(const char **pp, const char *e, unsigned char *out, int cap) {
    const char *p = pdf_skip_ws(*pp, e);
    if (p >= e || *p != '<') return -1;
    p++;
    int n = 0, hi = -1;
    for (; p < e && *p != '>'; p++) {
        int c = (unsigned char)*p;
        if (!isxdigit(c)) continue;
        int v = isdigit(c) ? c - '0' : (tolower(c) - 'a' + 10);
        if (hi < 0) hi = v;
        else { if (n < cap) out[n++] = (unsigned char)((hi << 4) | v); hi = -1; }
    }
    if (hi >= 0 && n < cap) out[n++] = (unsigned char)(hi << 4);
    *pp = (p < e) ? p + 1 : e;
    return n;
}
static uint32_t pdf_becode(const unsigned char *b, int n) {
    uint32_t v = 0;
    for (int i = 0; i < n && i < 4; i++) v = (v << 8) | b[i];
    return v;
}
/* UTF-16BE destination text (what /ToUnicode maps to) into UTF-8. */
static void pdf_u16(const unsigned char *s, int n, char *out, size_t cap) {
    size_t o = 0;
    for (int i = 0; i + 1 < n; i += 2) {
        uint32_t c = (uint32_t)(s[i] << 8 | s[i + 1]);
        if (c >= 0xd800 && c < 0xdc00 && i + 3 < n) {
            uint32_t lo = (uint32_t)(s[i + 2] << 8 | s[i + 3]);
            if (lo >= 0xdc00 && lo < 0xe000) {
                c = 0x10000 + ((c - 0xd800) << 10) + (lo - 0xdc00);
                i += 2;
            }
        }
        if (c == 0) continue;
        char tmp[4];
        int k = pdf_utf8(c, tmp);
        if (o + (size_t)k + 1 > cap) break;
        memcpy(out + o, tmp, (size_t)k);
        o += (size_t)k;
    }
    out[o] = 0;
}
static int pdf_uni_cmp(const void *a, const void *b) {
    uint32_t x = ((const PdfUni *)a)->code, y = ((const PdfUni *)b)->code;
    return x < y ? -1 : x > y;
}
static void pdf_uni_add(PdfFont *f, int *cap, uint32_t code, const char *u) {
    if (!*u) return;
    if (f->nuni == *cap) {
        *cap = *cap ? *cap * 2 : 128;
        f->uni = xrealloc(f->uni, (size_t)*cap * sizeof *f->uni);
    }
    f->uni[f->nuni].code = code;
    snprintf(f->uni[f->nuni].u, sizeof f->uni[f->nuni].u, "%s", u);
    f->nuni++;
}
/* Parse a /ToUnicode CMap: codespace width, bfchar and bfrange sections. */
static void pdf_cmap(PdfFont *f, const char *p, const char *e) {
    int cap = 0;
    while (p < e) {
        const char *q = p;
        while (q < e && !isalpha((unsigned char)*q) && *q != '_') q++;
        if (q >= e) break;
        const char *w = q;
        while (q < e && (isalnum((unsigned char)*q) || *q == '_')) q++;
        size_t wl = (size_t)(q - w);

        if (wl == 19 && !memcmp(w, "begincodespacerange", 19)) {
            unsigned char lo[16];
            const char *s = q;
            int n = pdf_hexstr(&s, e, lo, sizeof lo);
            if (n >= 2) f->twobyte = 1;
            p = q;
        } else if (wl == 11 && !memcmp(w, "beginbfchar", 11)) {
            const char *s = q;
            for (int i = 0; i < 65536; i++) {
                unsigned char src[8], dst[64];
                const char *t = pdf_skip_ws(s, e);
                if (t >= e || *t != '<') break;
                int sn = pdf_hexstr(&s, e, src, sizeof src);
                if (sn < 1) break;
                if (sn >= 2) f->twobyte = 1;
                t = pdf_skip_ws(s, e);
                char u[24] = "";
                if (t < e && *t == '<') {
                    int dn = pdf_hexstr(&s, e, dst, sizeof dst);
                    if (dn > 0) pdf_u16(dst, dn, u, sizeof u);
                } else {                        /* a glyph /name destination */
                    char nm[64];
                    const char *r = pdf_name(t, e, nm, sizeof nm);
                    if (!r) break;
                    s = r;
                    if (!strncmp(nm, "uni", 3) && strlen(nm) >= 7) {
                        unsigned c = (unsigned)strtol(nm + 3, NULL, 16);
                        u[pdf_utf8(c, u)] = 0;
                    } else if (!nm[1]) { u[0] = nm[0]; u[1] = 0; }
                }
                pdf_uni_add(f, &cap, pdf_becode(src, sn), u);
            }
            p = s;
        } else if (wl == 12 && !memcmp(w, "beginbfrange", 12)) {
            const char *s = q;
            for (int i = 0; i < 65536; i++) {
                unsigned char a[8], b[8], dst[64];
                const char *t = pdf_skip_ws(s, e);
                if (t >= e || *t != '<') break;
                int an = pdf_hexstr(&s, e, a, sizeof a);
                int bn = pdf_hexstr(&s, e, b, sizeof b);
                if (an < 1 || bn < 1) break;
                if (an >= 2) f->twobyte = 1;
                uint32_t lo = pdf_becode(a, an), hi = pdf_becode(b, bn);
                if (hi < lo || hi - lo > 65535) hi = lo;
                t = pdf_skip_ws(s, e);
                if (t < e && *t == '[') {              /* one destination each */
                    s = t + 1;
                    for (uint32_t c = lo; c <= hi; c++) {
                        const char *u2 = pdf_skip_ws(s, e);
                        if (u2 >= e || *u2 != '<') break;
                        int dn = pdf_hexstr(&s, e, dst, sizeof dst);
                        char u[24] = "";
                        if (dn > 0) pdf_u16(dst, dn, u, sizeof u);
                        pdf_uni_add(f, &cap, c, u);
                    }
                    const char *u2 = pdf_skip_ws(s, e);
                    if (u2 < e && *u2 == ']') s = u2 + 1;
                } else if (t < e && *t == '<') {       /* consecutive from base */
                    int dn = pdf_hexstr(&s, e, dst, sizeof dst);
                    if (dn < 2) continue;
                    for (uint32_t c = lo; c <= hi; c++) {
                        char u[24] = "";
                        pdf_u16(dst, dn, u, sizeof u);
                        pdf_uni_add(f, &cap, c, u);
                        for (int k = dn - 1; k >= 0; k--)   /* ++ the last unit */
                            if (++dst[k]) break;
                    }
                } else break;
            }
            p = s;
        } else p = q;
    }
    if (f->nuni > 1) qsort(f->uni, (size_t)f->nuni, sizeof *f->uni, pdf_uni_cmp);
}
/* CP1252's 0x80-0x9f block, the one range where it differs from Latin-1. */
static const uint16_t pdf_win1252[32] = {
    0x20ac, 0x0081, 0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021,
    0x02c6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008d, 0x017d, 0x008f,
    0x0090, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014,
    0x02dc, 0x2122, 0x0161, 0x203a, 0x0153, 0x009d, 0x017e, 0x0178,
};
static const char *pdf_glyph(PdfFont *f, uint32_t code, char *tmp) {
    if (f->nuni) {
        int lo = 0, hi = f->nuni - 1;
        while (lo <= hi) {
            int m = (lo + hi) / 2;
            if (f->uni[m].code == code) return f->uni[m].u;
            if (f->uni[m].code < code) lo = m + 1;
            else hi = m - 1;
        }
    }
    if (f->twobyte) return "";              /* a CID with no map means nothing */
    uint32_t u = code;
    if (code >= 0x80 && code < 0xa0) u = pdf_win1252[code - 0x80];
    if (u < 0x20 && u != '\t') return "";
    tmp[pdf_utf8(u, tmp)] = 0;
    return tmp;
}
static double pdf_width(PdfFont *f, uint32_t code) {
    if (f->twobyte) {
        for (int i = 0; i < f->ncw; i++)
            if (code >= f->cw[i].lo && code <= f->cw[i].hi) return f->cw[i].w;
        return f->dw;
    }
    int i = (int)code - f->firstchar;
    if (i >= 0 && i < f->nw && f->w[i] > 0) return f->w[i];
    return f->dw;
}
void pdf_font_free(PdfFont *f) {
    free(f->uni); free(f->w); free(f->cw);
    memset(f, 0, sizeof *f);
}
static void pdf_font_load(Pdf *pdf, PSpan fd, PdfFont *f) {
    memset(f, 0, sizeof *f);
    f->dw = 500;
    if (!fd.p) return;

    PSpan sub = pdf_dget(fd, "Subtype");
    PSpan src = fd;                       /* where the widths live */
    if (pdf_isname(sub, "Type0")) {
        f->twobyte = 1;
        f->dw = 1000;
        PSpan df = pdf_dgetr(pdf, fd, "DescendantFonts");
        const char *p = pdf_arr_open(df);
        PSpan d0;
        if (p && pdf_arr_next(&p, df.e, &d0)) src = pdf_get(pdf, d0);
        f->dw = pdf_dnum(pdf, src, "DW", 1000);

        PSpan wa = pdf_dgetr(pdf, src, "W");     /* [c [w…] | c1 c2 w]* */
        const char *q = pdf_arr_open(wa);
        int cap = 0;
        PSpan v;
        while (q && pdf_arr_next(&q, wa.e, &v)) {
            uint32_t c1 = (uint32_t)pdf_num(v, 0);
            const char *save = q;
            PSpan nx;
            if (!pdf_arr_next(&q, wa.e, &nx)) break;
            const char *inner = pdf_arr_open(nx);
            if (inner) {
                PSpan wv;
                for (uint32_t c = c1; pdf_arr_next(&inner, nx.e, &wv); c++) {
                    if (f->ncw == cap) {
                        cap = cap ? cap * 2 : 64;
                        f->cw = xrealloc(f->cw, (size_t)cap * sizeof *f->cw);
                    }
                    f->cw[f->ncw].lo = f->cw[f->ncw].hi = c;
                    f->cw[f->ncw].w = pdf_num(wv, f->dw);
                    f->ncw++;
                }
            } else {
                uint32_t c2 = (uint32_t)pdf_num(nx, c1);
                PSpan wv;
                if (!pdf_arr_next(&q, wa.e, &wv)) { q = save; break; }
                if (c2 < c1 || c2 - c1 > 65535) c2 = c1;
                if (f->ncw == cap) {
                    cap = cap ? cap * 2 : 64;
                    f->cw = xrealloc(f->cw, (size_t)cap * sizeof *f->cw);
                }
                f->cw[f->ncw].lo = c1;
                f->cw[f->ncw].hi = c2;
                f->cw[f->ncw].w = pdf_num(wv, f->dw);
                f->ncw++;
            }
        }
    } else {
        f->firstchar = (int)pdf_dnum(pdf, fd, "FirstChar", 0);
        PSpan wa = pdf_dgetr(pdf, fd, "Widths");
        const char *q = pdf_arr_open(wa);
        int cap = 0;
        PSpan v;
        while (q && pdf_arr_next(&q, wa.e, &v)) {
            if (f->nw == cap) {
                cap = cap ? cap * 2 : 128;
                f->w = xrealloc(f->w, (size_t)cap * sizeof *f->w);
            }
            f->w[f->nw++] = pdf_num(v, 0);
        }
        PSpan desc = pdf_dgetr(pdf, fd, "FontDescriptor");
        if (desc.p) f->dw = pdf_dnum(pdf, desc, "MissingWidth", f->nw ? 0 : 500);
        if (f->dw <= 0 && !f->nw) f->dw = 500;
    }
    PSpan tu = pdf_dgetr(pdf, fd, "ToUnicode");
    if (tu.p) {
        size_t n;
        char *cm = pdf_stream(pdf, tu, &n);
        if (cm) { pdf_cmap(f, cm, cm + n); free(cm); }
    }
}

/* ── content streams ─────────────────────────────────────────────── */
static void mmul(const double *a, const double *b, double *r) {   /* r = a × b */
    double t[6];
    t[0] = a[0]*b[0] + a[1]*b[2];
    t[1] = a[0]*b[1] + a[1]*b[3];
    t[2] = a[2]*b[0] + a[3]*b[2];
    t[3] = a[2]*b[1] + a[3]*b[3];
    t[4] = a[4]*b[0] + a[5]*b[2] + b[4];
    t[5] = a[4]*b[1] + a[5]*b[3] + b[5];
    memcpy(r, t, sizeof t);
}
double dabs(double v) { return v < 0 ? -v : v; }

static PdfFont *pdf_font_for(Pdf *pdf, PdfOut *o, PSpan res, const char *nm) {
    for (int i = 0; i < o->nfc; i++)
        if (o->fc[i].key == res.p && !strcmp(o->fc[i].nm, nm)) return &o->fc[i].f;
    int slot = o->nfc;
    if (slot == (int)(sizeof o->fc / sizeof *o->fc)) {   /* cache full: recycle */
        slot = 0;
        pdf_font_free(&o->fc[0].f);
    } else o->nfc++;
    o->fc[slot].key = res.p;
    snprintf(o->fc[slot].nm, sizeof o->fc[slot].nm, "%s", nm);
    PSpan fonts = pdf_dgetr(pdf, res, "Font");
    pdf_font_load(pdf, pdf_dgetr(pdf, fonts, nm), &o->fc[slot].f);
    return &o->fc[slot].f;
}
static void pdf_frag(PdfOut *o, double x, double y, double h, double w,
                     char *s, int len, int dir) {
    if (len <= 0 || o->nf >= PDF_MAXFRAG) { free(s); return; }
    if (o->nf == o->fcap) {
        o->fcap = o->fcap ? o->fcap * 2 : 512;
        o->f = xrealloc(o->f, (size_t)o->fcap * sizeof *o->f);
    }
    PFrag *g = &o->f[o->nf++];
    g->x = x; g->y = y; g->h = h; g->w = w; g->s = s; g->len = len; g->dir = dir;
}
/* Decode a literal (string) into bytes. */
static int pdf_litstr(const char *p, const char *e, unsigned char *out, int cap) {
    if (p >= e || *p != '(') return -1;
    p++;
    int n = 0, d = 1;
    while (p < e) {
        int c = (unsigned char)*p++;
        if (c == '\\') {
            if (p >= e) break;
            int x = (unsigned char)*p++;
            switch (x) {
                case 'n': c = '\n'; break;
                case 'r': c = '\r'; break;
                case 't': c = '\t'; break;
                case 'b': c = '\b'; break;
                case 'f': c = '\f'; break;
                case '\r': if (p < e && *p == '\n') p++;  continue;   /* line join */
                case '\n': continue;
                default:
                    if (x >= '0' && x <= '7') {
                        int v = x - '0';
                        for (int k = 0; k < 2 && p < e && *p >= '0' && *p <= '7'; k++)
                            v = v * 8 + (*p++ - '0');
                        c = v & 0xff;
                    } else c = x;
            }
        } else if (c == '(') { d++; }
        else if (c == ')') { if (--d == 0) break; }
        if (n < cap) out[n++] = (unsigned char)c;
    }
    return n;
}
/* Draw one string: append its glyphs and advance the text matrix. */
static void pdf_show(PdfOut *o, PdfFont *f, const unsigned char *s, int n,
                     double *tm, const double *ctm, double fs, double tc,
                     double tw, double th, double ts) {
    if (n <= 0) return;
    double trm[6] = { fs * th, 0, 0, fs, 0, ts }, m[6];
    mmul(trm, tm, m);
    mmul(m, ctm, m);
    double x0 = m[4], y0 = m[5];
    double scale = dabs(tm[0] * ctm[0]) + dabs(tm[1] * ctm[2]);
    if (scale < 1e-9) scale = 1;
    double h = fs * (dabs(tm[3] * ctm[3]) + dabs(tm[2] * ctm[1]));
    if (h < 0.01) h = fs;
    /* which way this baseline runs, from the transformed x-basis vector */
    int dir = (dabs(m[0]) >= dabs(m[1])) ? (m[0] >= 0 ? 0 : 2)
                                         : (m[1] >= 0 ? 1 : 3);

    char *buf = xmalloc((size_t)n * 5 + 8);
    int len = 0;
    double adv = 0;
    int step = f->twobyte ? 2 : 1;
    for (int i = 0; i + step <= n; i += step) {
        uint32_t code = f->twobyte ? (uint32_t)(s[i] << 8 | s[i + 1]) : s[i];
        char tmp[8];
        const char *g = pdf_glyph(f, code, tmp);
        for (const char *c = g; *c; c++) buf[len++] = *c;
        double w0 = pdf_width(f, code) / 1000.0;
        double a = (w0 * fs + tc + (code == 32 && step == 1 ? tw : 0)) * th;
        adv += a;
        double tr[6] = { 1, 0, 0, 1, a, 0 };
        mmul(tr, tm, tm);
    }
    buf[len] = 0;
    pdf_frag(o, x0, y0, h, adv * scale, buf, len, dir);
}

/* Recurse into a form XObject so text placed inside one is not lost. */
static void pdf_do_form(Pdf *pdf, PdfOut *o, PSpan res, const char *nm,
                        const double *ctm, int depth) {
    PSpan xo = pdf_dgetr(pdf, pdf_dgetr(pdf, res, "XObject"), nm);
    if (!xo.p || !pdf_isname(pdf_dget(xo, "Subtype"), "Form")) return;
    size_t n;
    char *body = pdf_stream(pdf, xo, &n);
    if (!body) return;
    double m[6] = { 1, 0, 0, 1, 0, 0 };
    PSpan mx = pdf_dgetr(pdf, xo, "Matrix");
    const char *q = pdf_arr_open(mx);
    if (q) {
        PSpan v;
        for (int i = 0; i < 6 && pdf_arr_next(&q, mx.e, &v); i++) m[i] = pdf_num(v, m[i]);
    }
    mmul(m, ctm, m);
    PSpan r2 = pdf_dgetr(pdf, xo, "Resources");
    pdf_run(pdf, body, body + n, r2.p ? r2 : res, m, o, depth + 1);
    free(body);
}
/* Interpret a content stream. Only the text and coordinate operators matter;
 * everything else is skipped, but inline images have to be stepped over
 * explicitly or their binary payload would lex as operators. */
void pdf_run(Pdf *pdf, const char *p, const char *e, PSpan res,
                    const double *ctm0, PdfOut *o, int depth) {
    if (depth > 8) return;
    double ctm[6];
    memcpy(ctm, ctm0, sizeof ctm);
    double gs[32][6];
    int ngs = 0;
    double tm[6] = { 1, 0, 0, 1, 0, 0 }, tlm[6] = { 1, 0, 0, 1, 0, 0 };
    double tc = 0, tw = 0, th = 1, tl = 0, ts = 0, fs = 0;
    PdfFont dummy;
    memset(&dummy, 0, sizeof dummy);
    dummy.dw = 500;
    PdfFont *font = &dummy;

    /* operand ring: the last few tokens seen before an operator */
    enum { NOP = 12 };
    PSpan ops[NOP];
    int nops = 0;

    while (p < e) {
        p = pdf_skip_ws(p, e);
        if (p >= e) break;
        int c = (unsigned char)*p;
        if (c == '/' || c == '(' || c == '<' || c == '[' || c == '{' ||
            isdigit(c) || c == '+' || c == '-' || c == '.') {
            const char *q = pdf_skip_obj(p, e);
            if (q <= p) break;
            if (nops == NOP) { memmove(ops, ops + 1, sizeof ops - sizeof *ops); nops--; }
            ops[nops].p = p;
            ops[nops].e = q;
            nops++;
            p = q;
            continue;
        }
        if (c == ']' || c == ')' || c == '>' || c == '}') { p++; continue; }

        const char *w = p;
        while (p < e && !pdf_ws((unsigned char)*p) && !pdf_delim((unsigned char)*p)) p++;
        size_t wl = (size_t)(p - w);
        if (wl == 0) { p++; continue; }
        #define OP(s) (wl == sizeof(s) - 1 && !memcmp(w, (s), sizeof(s) - 1))
        #define ARG(i) (nops > (i) ? ops[nops - 1 - (i)] : (PSpan){ NULL, NULL })

        if (OP("BT")) {
            double id[6] = { 1, 0, 0, 1, 0, 0 };
            memcpy(tm, id, sizeof tm);
            memcpy(tlm, id, sizeof tlm);
        } else if (OP("q")) {
            if (ngs < 32) memcpy(gs[ngs++], ctm, sizeof ctm);
        } else if (OP("Q")) {
            if (ngs > 0) memcpy(ctm, gs[--ngs], sizeof ctm);
        } else if (OP("cm") && nops >= 6) {
            double m[6];
            for (int i = 0; i < 6; i++) m[i] = pdf_num(ARG(5 - i), i == 0 || i == 3);
            mmul(m, ctm, ctm);
        } else if (OP("Tf") && nops >= 2) {
            char nm[64];
            fs = pdf_num(ARG(0), 0);
            if (pdf_name(ARG(1).p, ARG(1).e, nm, sizeof nm))
                font = pdf_font_for(pdf, o, res, nm);
        } else if (OP("Tc")) { tc = pdf_num(ARG(0), 0);
        } else if (OP("Tw")) { tw = pdf_num(ARG(0), 0);
        } else if (OP("Tz")) { th = pdf_num(ARG(0), 100) / 100.0;
        } else if (OP("TL")) { tl = pdf_num(ARG(0), 0);
        } else if (OP("Ts")) { ts = pdf_num(ARG(0), 0);
        } else if ((OP("Td") || OP("TD")) && nops >= 2) {
            double tx = pdf_num(ARG(1), 0), ty = pdf_num(ARG(0), 0);
            if (OP("TD")) tl = -ty;
            double m[6] = { 1, 0, 0, 1, tx, ty };
            mmul(m, tlm, tlm);
            memcpy(tm, tlm, sizeof tm);
        } else if (OP("Tm") && nops >= 6) {
            for (int i = 0; i < 6; i++) tlm[i] = pdf_num(ARG(5 - i), i == 0 || i == 3);
            memcpy(tm, tlm, sizeof tm);
        } else if (OP("T*") || OP("'") || OP("\"")) {
            if (OP("\"") && nops >= 3) { tw = pdf_num(ARG(2), tw); tc = pdf_num(ARG(1), tc); }
            double m[6] = { 1, 0, 0, 1, 0, -tl };
            mmul(m, tlm, tlm);
            memcpy(tm, tlm, sizeof tm);
            if (!OP("T*")) {
                unsigned char buf[4096];
                PSpan s = ARG(0);
                int n = s.p ? pdf_litstr(pdf_skip_ws(s.p, s.e), s.e, buf, sizeof buf) : -1;
                if (n > 0) pdf_show(o, font, buf, n, tm, ctm, fs, tc, tw, th, ts);
            }
        } else if (OP("Tj") && nops >= 1) {
            unsigned char buf[4096];
            PSpan s = ARG(0);
            const char *sp = pdf_skip_ws(s.p, s.e);
            int n = (sp && sp < s.e && *sp == '(') ? pdf_litstr(sp, s.e, buf, sizeof buf)
                                                   : pdf_hexstr(&sp, s.e, buf, sizeof buf);
            if (n > 0) pdf_show(o, font, buf, n, tm, ctm, fs, tc, tw, th, ts);
        } else if (OP("TJ") && nops >= 1) {
            PSpan a = ARG(0);
            const char *q = pdf_arr_open(a);
            PSpan v;
            while (q && pdf_arr_next(&q, a.e, &v)) {
                const char *sp = pdf_skip_ws(v.p, v.e);
                if (sp >= v.e) break;
                if (*sp == '(' || *sp == '<') {
                    unsigned char buf[4096];
                    int n = (*sp == '(') ? pdf_litstr(sp, v.e, buf, sizeof buf)
                                         : pdf_hexstr(&sp, v.e, buf, sizeof buf);
                    if (n > 0) pdf_show(o, font, buf, n, tm, ctm, fs, tc, tw, th, ts);
                } else {                                   /* kerning adjustment */
                    double adj = -pdf_num(v, 0) / 1000.0 * fs * th;
                    double m[6] = { 1, 0, 0, 1, adj, 0 };
                    mmul(m, tm, tm);
                }
            }
        } else if (OP("Do") && nops >= 1) {
            char nm[64];
            if (pdf_name(ARG(0).p, ARG(0).e, nm, sizeof nm))
                pdf_do_form(pdf, o, res, nm, ctm, depth);
        } else if (OP("BI")) {
            const char *q = p;                    /* skip to the matching EI */
            while (q + 2 <= e && !(q[0] == 'I' && q[1] == 'D' &&
                   (q + 2 == e || pdf_ws((unsigned char)q[2])))) q++;
            q = (q + 2 <= e) ? q + 3 : e;
            while (q + 2 <= e && !(pdf_ws((unsigned char)q[-1]) && q[0] == 'E' &&
                   q[1] == 'I' && (q + 2 == e || pdf_ws((unsigned char)q[2]) ||
                                   pdf_delim((unsigned char)q[2])))) q++;
            p = (q + 2 <= e) ? q + 2 : e;
        }
        #undef OP
        #undef ARG
        nops = 0;
    }
    pdf_font_free(&dummy);
}

/* ── layout ───────────────────────────────────────────────────────── */
static int pfrag_cmp(const void *a, const void *b) {
    const PFrag *x = a, *y = b;
    if (x->y != y->y) return x->y < y->y ? 1 : -1;     /* top of the page first */
    if (x->x != y->x) return x->x < y->x ? -1 : 1;
    return 0;
}
static int pfrag_xcmp(const void *a, const void *b) {
    const PFrag *x = a, *y = b;
    return x->x < y->x ? -1 : x->x > y->x;
}
static int dcmp(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}
static int pdf_nchars(const char *s, int len) {
    int n = 0;
    for (int i = 0; i < len; i++) if (((unsigned char)s[i] & 0xc0) != 0x80) n++;
    return n;
}
/* Rotate the page upright, sort into reading order, group rows and pick a
 * column unit. All of this is width-independent, so it happens once per page
 * and a resize only re-runs pdf_layout(). */
void pdf_prepare(PdfOut *o) {
    o->unit = 5.0;
    o->left = 0;
    o->nrow = 0;
    if (o->nf == 0) return;

    /* Turn the page so the dominant baseline direction reads left to right.
     * Weighting by character count keeps a sideways stamp or margin label
     * from out-voting the body text. */
    long vote[4] = { 0, 0, 0, 0 };
    for (int i = 0; i < o->nf; i++)
        vote[o->f[i].dir & 3] += pdf_nchars(o->f[i].s, o->f[i].len);
    int dom = 0;
    for (int d = 1; d < 4; d++) if (vote[d] > vote[dom]) dom = d;
    if (dom) {
        for (int i = 0; i < o->nf; i++) {
            double x = o->f[i].x, y = o->f[i].y;
            if (dom == 1)      { o->f[i].x = y;  o->f[i].y = -x; }
            else if (dom == 2) { o->f[i].x = -x; o->f[i].y = -y; }
            else               { o->f[i].x = -y; o->f[i].y = x;  }
        }
    }
    qsort(o->f, (size_t)o->nf, sizeof *o->f, pfrag_cmp);

    /* typical advance per character, as the column unit */
    double *samp = xmalloc((size_t)o->nf * sizeof *samp);
    int ns = 0;
    for (int i = 0; i < o->nf; i++) {
        int nc = pdf_nchars(o->f[i].s, o->f[i].len);
        if (nc > 0 && o->f[i].w > 0.01) samp[ns++] = o->f[i].w / nc;
    }
    if (ns) {
        qsort(samp, (size_t)ns, sizeof *samp, dcmp);
        o->unit = samp[ns / 2];
    }
    free(samp);
    if (o->unit < 0.5) o->unit = 0.5;

    o->left = o->f[0].x;
    for (int i = 1; i < o->nf; i++) if (o->f[i].x < o->left) o->left = o->f[i].x;

    /* group into rows by vertical proximity, then order each row by x */
    o->row = xmalloc((size_t)(o->nf + 1) * sizeof *o->row);
    for (int i = 0; i < o->nf; ) {
        double rowy = o->f[i].y, rowh = o->f[i].h;
        int j = i + 1;
        for (; j < o->nf; j++) {
            double tol = 0.4 * (o->f[j].h > rowh ? o->f[j].h : rowh);
            if (tol < 1.0) tol = 1.0;
            if (tol > 14.0) tol = 14.0;
            if (rowy - o->f[j].y > tol) break;
            if (o->f[j].h > rowh) rowh = o->f[j].h;
        }
        qsort(o->f + i, (size_t)(j - i), sizeof *o->f, pfrag_xcmp);
        o->row[o->nrow++] = i;
        i = j;
    }
    o->row[o->nrow] = o->nf;
}
/* Render one row into `line` at the given column unit. Used both to measure
 * the page's natural width and to emit it, so the two always agree. */
static int pdf_row_build(PdfOut *o, int k, double unit, char *line, int cap) {
    int len = 0;
    double pen = o->left;
    for (int i = o->row[k]; i < o->row[k + 1]; i++) {
        PFrag *g = &o->f[i];
        int col = (int)((g->x - o->left) / unit + 0.5);
        if (col < 0) col = 0;
        if (col > len) {
            int pad = min2(col, cap - 1) - len;
            for (int s = 0; s < pad; s++) line[len++] = ' ';
        } else if (len > 0 && line[len - 1] != ' ' && g->x - pen > unit * 0.28) {
            if (len < cap) line[len++] = ' ';
        }
        for (int s = 0; s < g->len && len < cap; s++) {
            char ch = g->s[s];
            line[len++] = (ch == '\n' || ch == '\r' || ch == '\f') ? ' ' : ch;
        }
        pen = g->x + g->w;
    }
    while (len > 0 && line[len - 1] == ' ') len--;
    return len;
}
/* Append a row, wrapping it to `width` so a narrow pane never needs sideways
 * scrolling. Continuation lines keep the row's own indent, which holds bullet
 * lists and indented blocks together. Breaking on bytes is deliberate: the
 * editor measures columns in bytes too, and a multi-byte character draws
 * narrower than it measures, so a byte-wrapped line always fits. */
static void pdf_emit(Buf *b, const char *s, int len, int width) {
    if (width <= 0 || len <= width) { buf_insert_line(b, b->n, s, len); return; }
    int lead = 0;
    while (lead < len && s[lead] == ' ') lead++;
    int cont = min2(lead, width / 3);
    char *tmp = xmalloc((size_t)width + 8);
    for (int start = 0, ind = 0; start < len; ) {
        int room = width - ind;
        if (room < 8) { ind = 0; room = width; }
        int take;
        if (len - start <= room) {
            take = len - start;
        } else {
            int brk = -1;
            for (int k = start + room; k > start; k--)
                if (s[k] == ' ') { brk = k; break; }
            if (brk > start) take = brk - start;
            else {                      /* one long word: split on a char edge */
                take = room;
                while (take > 1 && ((unsigned char)s[start + take] & 0xc0) == 0x80)
                    take--;
            }
        }
        int n = 0;
        for (int i = 0; i < ind; i++) tmp[n++] = ' ';
        memcpy(tmp + n, s + start, (size_t)take);
        n += take;
        while (n > 0 && tmp[n - 1] == ' ') n--;
        buf_insert_line(b, b->n, tmp, n);
        start += take;
        while (start < len && s[start] == ' ') start++;
        ind = cont;
    }
    free(tmp);
}
/* Does this row look like a table or a two-column layout rather than prose?
 * A run of three spaces inside the text means the fragments were placed apart
 * on purpose, and joining such rows into a paragraph would scramble them. */
static int pdf_row_tabular(const char *s, int len) {
    int i = 0;
    while (i < len && s[i] == ' ') i++;
    for (int run = 0; i < len; i++) {
        if (s[i] == ' ') { if (++run >= 3) return 1; }
        else run = 0;
    }
    return 0;
}
/* Fragments → text lines at a target width. Columns come from dividing the
 * horizontal offset by a typical character width, so tables and indentation
 * survive. When the page is wider than the pane the gaps are squeezed first;
 * if that is not enough, prose paragraphs are re-flowed as a whole rather
 * than wrapped line by line, which would leave a short orphan after every
 * source line. A page that already fits is emitted untouched.
 * width <= 0 means "no fitting at all". */
void pdf_layout(PdfOut *o, Buf *b, int width) {
    if (o->nf <= 0 || o->nrow <= 0 || o->nrow > o->nf || o->nf > PDF_MAXFRAG)
        return;
    char *line = xmalloc(PDF_MAXCOL + 8);
    double unit = o->unit;
    int narrowed = 0;

    if (width > 0) {
        /* Squeeze inter-column gaps until the widest row fits. Text itself
         * cannot shrink, so this converges on the all-text width; whatever is
         * still over gets re-flowed below. */
        for (int pass = 0; pass < 3; pass++) {
            int wide = 0;
            for (int k = 0; k < o->nrow; k++) {
                int n = pdf_row_build(o, k, unit, line, PDF_MAXCOL);
                if (n > wide) wide = n;
            }
            if (wide <= width) break;
            narrowed = 1;
            double grow = (double)wide / width;
            if (grow < 1.02) break;
            unit *= grow;
        }
    }
    /* materialise every row once: the join pass needs to look ahead */
    typedef struct { char *s; int len, col, tab; double y, h; } PRow;
    PRow *R = xmalloc((size_t)o->nrow * sizeof *R);
    int body = 1;
    for (int k = 0; k < o->nrow; k++) {
        int len = pdf_row_build(o, k, unit, line, PDF_MAXCOL);
        R[k].len = len;
        R[k].s = xmalloc((size_t)len + 1);
        memcpy(R[k].s, line, (size_t)len);
        R[k].s[len] = 0;
        int c = 0;
        while (c < len && line[c] == ' ') c++;
        R[k].col = c;
        R[k].tab = pdf_row_tabular(line, len);
        int i = o->row[k];
        R[k].y = o->f[i].y;
        R[k].h = o->f[i].h;
        for (int j = i + 1; j < o->row[k + 1]; j++)
            if (o->f[j].h > R[k].h) R[k].h = o->f[j].h;
        if (len > body) body = len;
    }
    free(line);

    char *para = xmalloc((size_t)PDF_MAXCOL * 4 + 8);
    for (int k = 0; k < o->nrow; ) {
        if (k) {                            /* keep paragraph breaks visible */
            double gap = R[k - 1].y - R[k].y;
            double ref = (R[k - 1].h > R[k].h ? R[k - 1].h : R[k].h);
            if (ref > 0.01 && gap > ref * 1.7) {
                buf_insert_line(b, b->n, "", 0);
                if (gap > ref * 4.0) buf_insert_line(b, b->n, "", 0);
            }
        }
        int plen = R[k].len;
        memcpy(para, R[k].s, (size_t)plen);
        int last = k;
        /* Pull following rows into this paragraph only when we had to narrow
         * the page — at full width the PDF's own line breaks are the truth. */
        while (narrowed && !R[last].tab && last + 1 < o->nrow) {
            int n = last + 1;
            double gap = R[last].y - R[n].y;
            double ref = (R[last].h > R[n].h ? R[last].h : R[n].h);
            int indent_ok = (n == k + 1) ? (R[n].col <= R[k].col + 1)
                                         : (R[n].col == R[last].col);
            double hr = R[n].h > 0.01 ? R[last].h / R[n].h : 1.0;
            if (R[n].tab || R[n].len == 0) break;
            if (ref > 0.01 && gap > ref * 1.7) break;      /* blank line ahead */
            if (!indent_ok) break;
            if (hr < 0.8 || hr > 1.25) break;              /* heading, not body */
            if (R[last].len < body * 72 / 100) break;      /* short = para end */
            int add = R[n].len - R[n].col;
            if (plen + 1 + add > PDF_MAXCOL * 4) break;
            para[plen++] = ' ';
            memcpy(para + plen, R[n].s + R[n].col, (size_t)add);
            plen += add;
            last = n;
        }
        pdf_emit(b, para, plen, width);
        for (int i = k; i <= last; i++) free(R[i].s);
        k = last + 1;
    }
    free(para);
    free(R);
}
