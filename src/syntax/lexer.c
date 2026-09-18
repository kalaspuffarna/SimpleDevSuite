#include "core/sds.h"
#include "syntax/syntax.h"
#include "syntax/syntax_internal.h"
#include "markdown/markdown.h"

/* ── lexer ────────────────────────────────────────────────────────── */
/* The keyword lists are written as readable space-delimited strings, but
 * scanning them with strstr for every identifier on every visible line got
 * expensive as the lists grew. Split them once into sorted arrays and binary
 * search instead, so adding keywords stays free. */
typedef struct { const char *w; int len; } Kw;
static struct { Kw *kw; int nkw; Kw *ty; int nty; } kwidx[NLANGS];
static int kw_cmp(const void *a, const void *b) {
    const Kw *x = a, *y = b;
    int n = x->len < y->len ? x->len : y->len;
    int c = memcmp(x->w, y->w, (size_t)n);
    if (c) return c;
    return x->len - y->len;
}
static void kw_split(const char *src, Kw **out, int *nout) {
    int n = 0, cap = 16;
    Kw *v = xmalloc((size_t)cap * sizeof *v);
    const char *p = src;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        const char *s = p;
        while (*p && *p != ' ') p++;
        if (n == cap) { cap *= 2; v = xrealloc(v, (size_t)cap * sizeof *v); }
        v[n].w = s;
        v[n].len = (int)(p - s);
        n++;
    }
    qsort(v, (size_t)n, sizeof *v, kw_cmp);
    *out = v; *nout = n;
}
void kw_index_build(void) {
    for (int i = 0; i < NLANGS; i++) {
        kw_split(langs[i].kw,    &kwidx[i].kw, &kwidx[i].nkw);
        kw_split(langs[i].types, &kwidx[i].ty, &kwidx[i].nty);
    }
}
static int kw_class(const Lang *lg, const char *s, int len) {
    char buf[64];
    if (len > 63) return HA_DEF;
    /* nocase languages (SQL) keep their tables lowercase, so fold the token */
    if (lg->nocase) {
        for (int i = 0; i < len; i++) buf[i] = (char)tolower((unsigned char)s[i]);
        s = buf;
    }
    int li = (int)(lg - langs);
    if (li < 0 || li >= NLANGS) return HA_DEF;
    Kw key = { s, len };
    if (kwidx[li].nkw && bsearch(&key, kwidx[li].kw, (size_t)kwidx[li].nkw,
                                 sizeof(Kw), kw_cmp)) return HA_KW;
    if (kwidx[li].nty && bsearch(&key, kwidx[li].ty, (size_t)kwidx[li].nty,
                                 sizeof(Kw), kw_cmp)) return HA_TYPE;
    return HA_DEF;
}
/* Does `tok` (of known length `tl`) start at s[i]? The length is passed in
 * because the caller hoists it out of the loop: measuring the comment and
 * string delimiters with strlen for every character of every line made the
 * lexer cost O(len × Σ delimiter lengths) instead of O(len), and it was the
 * single most expensive thing about drawing a long line. */
static int tok_at(const char *s, int len, int i, const char *tok, int tl) {
    return tl && s[i] == tok[0] && i + tl <= len &&
           memcmp(s + i, tok, (size_t)tl) == 0;
}
/* lex one line; fills attr[0..len) if attr != NULL; returns end state */
/* Markdown in the *source* view. The keyword lexer has nothing useful to say
 * about prose, so markdown gets its own line pass — enough to see the
 * structure while editing — carrying the fenced-code state across lines in
 * the same `hst` field every other language uses. */
static int md_lex(const char *s, int len, int st, unsigned char *attr) {
#define MDA(i, a) do { if (attr) attr[i] = (unsigned char)(a); } while (0)
    if (attr) memset(attr, HA_DEF, (size_t)len);
    int i = 0;
    while (i < len && (s[i] == ' ' || s[i] == '\t')) i++;
    char fc; int fl;
    if (md_is_fence(s + i, len - i, &fc, &fl)) {
        for (int k = 0; k < len; k++) MDA(k, HA_STR);
        return st == ST_MDFENCE ? ST_NORM : ST_MDFENCE;
    }
    if (st == ST_MDFENCE) {
        for (int k = 0; k < len; k++) MDA(k, HA_STR);
        return ST_MDFENCE;
    }
    int lvl, ts, tl;
    if (md_atx(s + i, len - i, &lvl, &ts, &tl) || md_is_hr(s + i, len - i) ||
        md_setext(s + i, len - i)) {
        for (int k = i; k < len; k++) MDA(k, HA_PRE);
        return ST_NORM;
    }
    if (i < len && s[i] == '>') {
        for (int k = i; k < len; k++) MDA(k, HA_COM);
        return ST_NORM;
    }
    int mlen, ord;
    if (md_bullet(s + i, len - i, &mlen, &ord))
        for (int k = i; k < i + mlen && k < len; k++) MDA(k, HA_KW);
    for (int k = i; k < len; ) {
        if (s[k] == '\\') { k += 2; continue; }
        if (s[k] == '`') {                                    /* code span */
            int run = md_run_of(s, len, k, '`'), j = k + run, close = -1;
            while (j < len) {
                if (s[j] != '`') { j++; continue; }
                int r2 = md_run_of(s, len, j, '`');
                if (r2 == run) { close = j; break; }
                j += r2;
            }
            int e = close < 0 ? k + run : close + run;
            for (int q = k; q < e; q++) MDA(q, HA_STR);
            k = e;
            continue;
        }
        if (s[k] == '*' || s[k] == '_') {                      /* emphasis */
            int run = md_run_of(s, len, k, s[k]);
            int want = run >= 2 ? 2 : 1, close = -1;
            if (!(s[k] == '_' && k > i && word_ch((unsigned char)s[k-1])) &&
                k + want < len && !isspace((unsigned char)s[k + want]))
                for (int j = k + want; j < len; ) {
                    if (s[j] != s[k]) { j++; continue; }
                    int r2 = md_run_of(s, len, j, s[k]);
                    if (r2 >= want && !isspace((unsigned char)s[j-1])) { close = j; break; }
                    j += r2;
                }
            if (close < 0) { k += run; continue; }
            for (int q = k; q < close + want; q++) MDA(q, HA_KW);
            k = close + want;
            continue;
        }
        if (s[k] == '[') {                                         /* link */
            const char *e = memchr(s + k, ']', (size_t)(len - k));
            if (!e) { k++; continue; }
            int close = (int)(e - s);
            for (int q = k; q <= close; q++) MDA(q, HA_TYPE);
            k = close + 1;
            if (k < len && (s[k] == '(' || s[k] == '[')) {
                char shut = s[k] == '(' ? ')' : ']';
                const char *e2 = memchr(s + k, shut, (size_t)(len - k));
                int stop = e2 ? (int)(e2 - s) : len - 1;
                for (int q = k; q <= stop; q++) MDA(q, HA_COM);
                k = stop + 1;
            }
            continue;
        }
        k++;
    }
    return ST_NORM;
#undef MDA
}
/* `limit` is how many bytes of `attr` the caller will actually look at. The
 * lexer still has to start at the line's beginning — that is what `st` is
 * for — but it can stop once it is past what will be drawn, which is the
 * difference between a 15000-column line costing a screenful of work and
 * costing the whole line. With limit < len the return value means nothing;
 * callers that need the end state (ensure_hl) pass limit = len. */
static int lex_line(const Lang *lg, const char *s, int len, int st,
                    unsigned char *attr, int limit) {
#define SETA(i, a) do { if (attr) attr[i] = (unsigned char)(a); } while (0)
    if (len > limit) len = limit;
    if (lg->md) return md_lex(s, len, st, attr);
    const int lcl = (int)strlen(lg->lc),  lc2l = (int)strlen(lg->lc2);
    const int bol = (int)strlen(lg->bo),  bcl  = (int)strlen(lg->bc);
    const int t1l = (int)strlen(lg->t1),  t2l  = (int)strlen(lg->t2);
    int i = 0;
    if (attr) memset(attr, HA_DEF, (size_t)len);
    /* resume a multi-line construct */
    while (i < len && st != ST_NORM) {
        const char *end = st == ST_BCOM ? lg->bc : st == ST_TRI1 ? lg->t1 : lg->t2;
        int endl = st == ST_BCOM ? bcl : st == ST_TRI1 ? t1l : t2l;
        int a = st == ST_BCOM ? HA_COM : HA_STR;
        if (tok_at(s, len, i, end, endl)) {
            for (int k = 0; k < endl; k++) SETA(i + k, a);
            i += endl;
            st = ST_NORM;
        } else { SETA(i, a); i++; }
    }
    if (st != ST_NORM) return st;   /* whole line consumed */
    /* preprocessor line */
    if (lg->preproc) {
        int j = 0;
        while (j < len && isspace((unsigned char)s[j])) j++;
        if (j < len && s[j] == '#') {
            for (int k = j; k < len; k++) SETA(k, HA_PRE);
            return ST_NORM;
        }
    }
    while (i < len) {
        unsigned char c = (unsigned char)s[i];
        /* line comments */
        if (tok_at(s, len, i, lg->lc, lcl) || tok_at(s, len, i, lg->lc2, lc2l)) {
            for (int k = i; k < len; k++) SETA(k, HA_COM);
            return ST_NORM;
        }
        /* block comment open */
        if (tok_at(s, len, i, lg->bo, bol)) {
            for (int k = 0; k < bol; k++) SETA(i + k, HA_COM);
            i += bol;
            st = ST_BCOM;
            while (i < len) {
                if (tok_at(s, len, i, lg->bc, bcl)) {
                    for (int k = 0; k < bcl; k++) SETA(i + k, HA_COM);
                    i += bcl; st = ST_NORM; break;
                }
                SETA(i, HA_COM); i++;
            }
            if (st == ST_BCOM) return ST_BCOM;
            continue;
        }
        /* triple-quoted strings (python) */
        if (tok_at(s, len, i, lg->t1, t1l) || tok_at(s, len, i, lg->t2, t2l)) {
            int one = tok_at(s, len, i, lg->t1, t1l);
            const char *d = one ? lg->t1 : lg->t2;
            int dl = one ? t1l : t2l;
            for (int k = 0; k < dl; k++) SETA(i + k, HA_STR);
            i += dl;
            st = one ? ST_TRI1 : ST_TRI2;
            while (i < len) {
                if (tok_at(s, len, i, d, dl)) {
                    for (int k = 0; k < dl; k++) SETA(i + k, HA_STR);
                    i += dl; st = ST_NORM; break;
                }
                SETA(i, HA_STR); i++;
            }
            if (st != ST_NORM) return st;
            continue;
        }
        /* strings */
        if (c == '"' || (c == '`' && lg->bq) || (c == '\'' && lg->sq == 2)) {
            char q = (char)c;
            SETA(i, HA_STR); i++;
            while (i < len) {
                SETA(i, HA_STR);
                if (s[i] == '\\' && i + 1 < len) { SETA(i + 1, HA_STR); i += 2; continue; }
                if (s[i] == q) { i++; break; }
                i++;
            }
            continue;
        }
        /* char literals: 'x' or '\x' */
        if (c == '\'' && lg->sq == 1) {
            int close = -1;
            if (i + 2 < len && s[i+1] == '\\' && s[i+3] == '\'') close = i + 3;
            else if (i + 2 < len && s[i+2] == '\'') close = i + 2;
            if (close > 0) {
                for (int k = i; k <= close; k++) SETA(k, HA_STR);
                i = close + 1;
            } else i++;                 /* lifetime / apostrophe */
            continue;
        }
        /* numbers */
        if (isdigit(c)) {
            int j = i;
            while (j < len && (isalnum((unsigned char)s[j]) || s[j] == '.' || s[j] == '_'))
                j++;
            for (int k = i; k < j; k++) SETA(k, HA_NUM);
            i = j;
            continue;
        }
        /* identifiers / keywords */
        if (word_ch(c) && !isdigit(c)) {
            int j = i;
            while (j < len && word_ch((unsigned char)s[j])) j++;
            int cls = kw_class(lg, s + i, j - i);
            for (int k = i; k < j; k++) SETA(k, cls);
            i = j;
            continue;
        }
        i++;
    }
    return ST_NORM;
#undef SETA
}
/* Make hst valid for lines [0..upto].
 *
 * An edit only really disturbs the state chain until it converges again: type
 * a character on line 400 and line 401's start state is almost always what it
 * was, and every line below it is then still correct. `hl_dirty` is the last
 * line an edit touched and `hl_seen` how far a real lex pass has ever
 * reached, so past the edit this can stop at the first line whose state comes
 * out unchanged instead of re-lexing to the bottom of the screen. */
void ensure_hl(Buf *b, int upto) {
    if (upto >= b->n) upto = b->n - 1;
    if (b->hl_upto == 0) b->ln[0].hst = ST_NORM;
    for (int i = b->hl_upto; i < upto; i++) {
        int st = lex_line(b->lang, b->ln[i].s, b->ln[i].len, b->ln[i].hst,
                          NULL, b->ln[i].len);
        if (i >= b->hl_dirty && i + 1 <= b->hl_seen && b->ln[i + 1].hst == st) {
            b->hl_upto = upto;          /* the chain below is already right */
            b->hl_dirty = 0;
            return;
        }
        b->ln[i + 1].hst = st;
        if (i + 1 > b->hl_seen) b->hl_seen = i + 1;
    }
    if (upto > b->hl_upto) b->hl_upto = upto;
    b->hl_dirty = 0;
}

/* One line's syntax attributes, from tree-sitter when a grammar is installed
 * for this language and from the built-in lexer otherwise. */
void hl_line(Buf *b, int li, unsigned char *attr, int limit) {
    Line *l = &b->ln[li];
    if (limit > l->len) limit = l->len;
#ifdef SDS_TREESITTER
    if (ts_line_attrs(b, li, attr, limit)) return;
#endif
    lex_line(b->lang, l->s, l->len, l->hst, attr, limit);
}
