#include "core/sds.h"
#include "markdown/markdown.h"

/* config [markdown] */
int cfg_md_preview = 0;      /* config [markdown] preview: open rendered */
int cfg_md_width = 0;        /* config [markdown] width, 0 = the pane's */
/* ── markdown rendering ───────────────────────────────────────────────
 * A .md file opens as an ordinary editable buffer; the markdown key swaps
 * the pane over to a rendered view of it. That view is a second, read-only
 * copy of the text — styled characters instead of markup — laid out for the
 * width of the pane it is shown in and rebuilt whenever the buffer changes
 * or the pane resizes.
 *
 * One byte of style per character: three bits of color role and five of
 * terminal attributes, so "bold inside a link inside a quote" composes
 * without a combinatorial table of styles. draw_md turns those bytes into
 * curses attributes — see md_attr, next to the editor's attr_for.
 *
 * The parser is a pragmatic subset of CommonMark: headings (both kinds),
 * fenced and indented code, quotes, nested lists, tables, thematic breaks,
 * YAML front matter, and the usual inline run of emphasis, code spans and
 * links. What it doesn't recognise it prints as plain text, which is what
 * markdown itself does with anything it doesn't recognise.                */

enum { MC_TEXT, MC_HEAD, MC_CODE, MC_LINK, MC_QUOTE, MC_META, MC_MARK, MC_RULE };
#define MST(c, f)     (unsigned char)(((c) << 5) | (f))
#define MS_COL(st, c) (unsigned char)(((c) << 5) | ((st) & 31))
#define MS_ADD(st, f) (unsigned char)((st) | (f))

#define MD_MAXDEPTH 6          /* nesting the block parser will follow */
#define MD_MAXCOL  16          /* columns a table can have */

/* a stretch of styled text, built up before it is wrapped into lines */
typedef struct { char *s; unsigned char *a; int n, cap; } MdRun;

static void mr_room(MdRun *r, int need) {
    if (r->n + need <= r->cap) return;
    r->cap = (r->n + need) * 2 + 64;
    r->s = xrealloc(r->s, (size_t)r->cap);
    r->a = xrealloc(r->a, (size_t)r->cap);
}
static void mr_add(MdRun *r, const char *s, int n, unsigned char st) {
    if (n <= 0) return;
    mr_room(r, n);
    memcpy(r->s + r->n, s, (size_t)n);
    memset(r->a + r->n, st, (size_t)n);
    r->n += n;
}
static void mr_str(MdRun *r, const char *s, unsigned char st) {
    mr_add(r, s, (int)strlen(s), st);
}
static void mr_rep(MdRun *r, const char *glyph, int times, unsigned char st) {
    int n = (int)strlen(glyph);
    for (int i = 0; i < times; i++) mr_add(r, glyph, n, st);
}
static void mr_slice(MdRun *d, const MdRun *s, int from, int to) {
    if (to <= from) return;
    mr_room(d, to - from);
    memcpy(d->s + d->n, s->s + from, (size_t)(to - from));
    memcpy(d->a + d->n, s->a + from, (size_t)(to - from));
    d->n += to - from;
}
static void mr_cat(MdRun *d, const MdRun *s) { mr_slice(d, s, 0, s->n); }
static void mr_free(MdRun *r) {
    free(r->s); free(r->a);
    r->s = NULL; r->a = NULL; r->n = r->cap = 0;
}
/* columns a byte range occupies — continuation bytes don't take one */
int md_cols(const char *s, int n) {
    int k = 0;
    for (int i = 0; i < n; i++) if (((unsigned char)s[i] & 0xc0) != 0x80) k++;
    return k;
}
/* byte offset of column k */
int md_byte_at(const char *s, int n, int k) {
    int i = 0;
    while (i < n && k > 0) {
        i++;
        while (i < n && ((unsigned char)s[i] & 0xc0) == 0x80) i++;
        k--;
    }
    return i;
}

static void md_push(Md *m, const char *s, const unsigned char *a, int n, int src) {
    if (m->n == m->cap) {
        m->cap = m->cap ? m->cap * 2 : 128;
        m->ln  = xrealloc(m->ln,  (size_t)m->cap * sizeof *m->ln);
        m->at  = xrealloc(m->at,  (size_t)m->cap * sizeof *m->at);
        m->src = xrealloc(m->src, (size_t)m->cap * sizeof *m->src);
    }
    Line *l = &m->ln[m->n];
    l->s = xmalloc((size_t)n + 1);
    l->s[n] = 0;
    l->len = n;
    l->cap = n + 1;
    l->hst = 0;
    m->at[m->n] = xmalloc((size_t)n + 1);
    if (n) {                        /* an empty line has no buffer to copy */
        memcpy(l->s, s, (size_t)n);
        memcpy(m->at[m->n], a, (size_t)n);
    }
    m->src[m->n] = src;
    m->n++;
}
static void md_clear(Md *m) {
    for (int i = 0; i < m->n; i++) { free(m->ln[i].s); free(m->at[i]); }
    m->n = 0;
}
void md_free(Md *m) {
    if (!m) return;
    md_clear(m);
    free(m->ln); free(m->at); free(m->src);
    free(m);
}

/* ── inline markup ────────────────────────────────────────────────── */
/* plain substring search; the GNU memmem isn't in scope under _XOPEN_SOURCE */
static const char *md_find(const char *h, int hn, const char *n, int nn) {
    for (int i = 0; i + nn <= hn; i++)
        if (memcmp(h + i, n, (size_t)nn) == 0) return h + i;
    return NULL;
}
int md_run_of(const char *s, int len, int i, char c) {
    int n = 0;
    while (i + n < len && s[i + n] == c) n++;
    return n;
}
static void md_inline(MdRun *o, const char *s, int len, unsigned char base, int depth);

/* [text](url "title") — emit the text, then the target when it adds
 * something. A terminal can't be clicked, so a link that hides its URL is a
 * dead end; one that shows it is at least copyable. */
static void md_link(MdRun *o, const char *txt, int tl, const char *url, int ul,
                    unsigned char base, int depth) {
    while (ul > 0 && (url[0] == ' ' || url[0] == '<')) { url++; ul--; }
    while (ul > 0 && (url[ul-1] == ' ' || url[ul-1] == '>')) ul--;
    for (int q = 0; q + 1 < ul; q++)             /* drop a trailing title */
        if (url[q] == ' ' && (url[q+1] == '"' || url[q+1] == '\'')) { ul = q; break; }
    md_inline(o, txt, tl, MS_ADD(MS_COL(base, MC_LINK), MS_UNDER), depth + 1);
    if (ul > 0 && !(ul == tl && memcmp(url, txt, (size_t)ul) == 0)) {
        unsigned char st = MS_ADD(MS_COL(base, MC_META), MS_DIM);
        mr_str(o, " (", st);
        mr_add(o, url, ul, st);
        mr_str(o, ")", st);
    }
}
static void md_inline(MdRun *o, const char *s, int len, unsigned char base, int depth) {
    if (depth > MD_MAXDEPTH) { mr_add(o, s, len, base); return; }
    for (int i = 0; i < len; ) {
        char c = s[i];
        if (c == '\\' && i + 1 < len && strchr("\\`*_{}[]()#+-.!|~<>\"", s[i+1])) {
            mr_add(o, s + i + 1, 1, base);
            i += 2;
            continue;
        }
        if (c == '`') {                                    /* code span */
            int run = md_run_of(s, len, i, '`'), j = i + run, close = -1;
            while (j < len) {
                if (s[j] != '`') { j++; continue; }
                int r2 = md_run_of(s, len, j, '`');
                if (r2 == run) { close = j; break; }
                j += r2;
            }
            if (close < 0) { mr_add(o, s + i, run, base); i += run; continue; }
            int a = i + run, z = close;
            if (z - a >= 2 && s[a] == ' ' && s[z-1] == ' ') { a++; z--; }
            mr_add(o, s + a, z - a, MS_COL(base, MC_CODE));
            i = close + run;
            continue;
        }
        if (c == '<') {                       /* autolink, or an HTML tag */
            const char *gt = memchr(s + i + 1, '>', (size_t)(len - i - 1));
            int n = gt ? (int)(gt - (s + i + 1)) : 0;
            if (gt && n > 0 && !memchr(s + i + 1, ' ', (size_t)n) &&
                (md_find(s + i + 1, n, "://", 3) ||
                 memchr(s + i + 1, '@', (size_t)n))) {
                mr_add(o, s + i + 1, n, MS_ADD(MS_COL(base, MC_LINK), MS_UNDER));
                i += n + 2;
                continue;
            }
            /* a real tag carries no meaning in a terminal — drop it */
            if (gt && (isalpha((unsigned char)s[i+1]) || s[i+1] == '/' || s[i+1] == '!')) {
                i += n + 2;
                continue;
            }
        }
        if (c == '!' && i + 1 < len && s[i+1] == '[') {      /* image */
            const char *close = memchr(s + i + 2, ']', (size_t)(len - i - 2));
            if (close && close + 1 < s + len && close[1] == '(') {
                const char *end = memchr(close + 2, ')', (size_t)(len - (close + 2 - s)));
                if (end) {
                    unsigned char st = MS_ADD(MS_COL(base, MC_META), MS_DIM);
                    mr_str(o, "[image", st);
                    if (close > s + i + 2) {
                        mr_str(o, ": ", st);
                        mr_add(o, s + i + 2, (int)(close - (s + i + 2)), st);
                    }
                    mr_str(o, "]", st);
                    i = (int)(end - s) + 1;
                    continue;
                }
            }
        }
        if (c == '[') {                                       /* link */
            int j = i + 1, nest = 1;
            while (j < len && nest) {
                if (s[j] == '\\') j++;
                else if (s[j] == '[') nest++;
                else if (s[j] == ']') nest--;
                j++;
            }
            if (nest == 0) {
                const char *txt = s + i + 1;
                int tl = (int)((s + j - 1) - txt);
                if (j < len && s[j] == '(') {
                    int k = j + 1, par = 1;
                    while (k < len && par) {
                        if (s[k] == '(') par++;
                        else if (s[k] == ')') par--;
                        k++;
                    }
                    if (par == 0) {
                        md_link(o, txt, tl, s + j + 1, (int)((s + k - 1) - (s + j + 1)),
                                base, depth);
                        i = k;
                        continue;
                    }
                }
                /* [text][ref] and [ref] — the target lives elsewhere in the
                 * file, so show the text and leave the brackets out */
                int k = j;
                if (k < len && s[k] == '[') {
                    const char *e = memchr(s + k, ']', (size_t)(len - k));
                    if (e) k = (int)(e - s) + 1;
                }
                if (k > j || (j < len && s[j] != ':')) {
                    md_inline(o, txt, tl, MS_ADD(MS_COL(base, MC_LINK), MS_UNDER),
                              depth + 1);
                    i = k;
                    continue;
                }
            }
        }
        if (c == '~' && i + 1 < len && s[i+1] == '~') {        /* strikethrough */
            const char *e = md_find(s + i + 2, len - i - 2, "~~", 2);
            if (e) {
                md_inline(o, s + i + 2, (int)(e - (s + i + 2)),
                          MS_ADD(base, MS_DIM), depth + 1);
                i = (int)(e - s) + 2;
                continue;
            }
        }
        if (c == '*' || c == '_') {                            /* emphasis */
            int run = md_run_of(s, len, i, c);
            int want = run >= 2 ? 2 : 1;
            /* snake_case is not emphasis, so an underscore only opens one at
             * a word boundary; an opener also can't be followed by a space */
            int ok = !(c == '_' && i > 0 && word_ch((unsigned char)s[i-1])) &&
                     i + want < len && !isspace((unsigned char)s[i + want]);
            int close = -1;
            for (int j = i + want; ok && j < len; ) {
                if (s[j] == '\\') { j += 2; continue; }
                if (s[j] != c) { j++; continue; }
                int r2 = md_run_of(s, len, j, c);
                if (r2 >= want && !isspace((unsigned char)s[j-1])) { close = j; break; }
                j += r2;
            }
            if (close > 0) {
                md_inline(o, s + i + want, close - (i + want),
                          MS_ADD(base, want == 2 ? MS_BOLD : MS_ITAL), depth + 1);
                i = close + want;
                continue;
            }
            mr_add(o, s + i, run, base);
            i += run;
            continue;
        }
        int adv = 1;
        while (i + adv < len && ((unsigned char)s[i + adv] & 0xc0) == 0x80) adv++;
        mr_add(o, s + i, adv, base);
        i += adv;
    }
}

/* ── block markup ─────────────────────────────────────────────────── */
/* One source line, already detached from the buffer so a nested block can be
 * re-indented without touching the file. */
typedef struct { const char *s; int len, src; } MdSrc;

/* Where output goes. `pre` prefixes the first line this context emits and
 * `cont` every line after it, which is what puts a bullet on an item's first
 * row and spaces under it. A nested block builds its prefixes on top of its
 * parent's, taking the parent's unspent `pre` with it — an item's bullet
 * belongs on the first line of whatever is nested inside the item, since
 * that is the line that gets printed first. */
typedef struct {
    Md    *m;
    int    width;
    MdRun  pre, cont;
    int    pw;                 /* columns the prefix takes */
    int    spent;              /* has `pre` been printed yet? */
    int    ldepth;             /* lists nested so far, for the bullet glyph */
    unsigned char base;        /* style prose starts from, e.g. inside a quote */
} MdCtx;

static void md_ctx_child(MdCtx *out, MdCtx *par, const char *first,
                         const char *rest, unsigned char st) {
    memset(out, 0, sizeof *out);
    out->m = par->m;
    out->width = par->width;
    out->ldepth = par->ldepth;
    out->base = par->base;
    mr_cat(&out->pre, par->spent ? &par->cont : &par->pre);
    par->spent = 1;
    mr_str(&out->pre, first, st);
    mr_cat(&out->cont, &par->cont);
    mr_str(&out->cont, rest, st);
    /* both prefixes must be the same width or a wrapped line would jump */
    int a = md_cols(out->pre.s, out->pre.n), b = md_cols(out->cont.s, out->cont.n);
    for (; b < a; b++) mr_str(&out->cont, " ", st);
    for (; a < b; a++) mr_str(&out->pre, " ", st);
    out->pw = a > b ? a : b;
}
static void md_ctx_free(MdCtx *c) { mr_free(&c->pre); mr_free(&c->cont); }

/* Wrap `body` to the context width and push it out, prefix included. */
static void md_out(MdCtx *c, MdRun *body, int src) {
    static MdRun line;                       /* one scratch line, reused */
    int avail = c->width - c->pw;
    if (avail < 8) avail = 8;
    int n = body->n;
    while (n > 0 && body->s[n-1] == ' ') n--;
    for (int i = 0;;) {
        int j = i, cols = 0, brk = -1;
        while (j < n && cols < avail) {
            if (body->s[j] == ' ' && j > i) brk = j;
            j++;
            while (j < n && ((unsigned char)body->s[j] & 0xc0) == 0x80) j++;
            cols++;
        }
        int take = (j < n && brk > i) ? brk : j;
        int e = take;
        while (e > i && body->s[e-1] == ' ') e--;
        line.n = 0;
        mr_cat(&line, c->spent ? &c->cont : &c->pre);
        c->spent = 1;
        mr_slice(&line, body, i, e);
        md_push(c->m, line.s, line.a, line.n, src);
        i = take;
        while (i < n && body->s[i] == ' ') i++;
        if (i >= n) break;
    }
}
/* Push a line that must not be re-wrapped (rules, code, table rows). */
static void md_raw(MdCtx *c, MdRun *body, int src) {
    static MdRun line;
    line.n = 0;
    mr_cat(&line, c->spent ? &c->cont : &c->pre);
    c->spent = 1;
    mr_cat(&line, body);
    md_push(c->m, line.s, line.a, line.n, src);
}
static void md_blank(MdCtx *c, int src) {
    if (c->m->n) {                                   /* never two in a row */
        Line *l = &c->m->ln[c->m->n - 1];
        int i = 0;
        while (i < l->len && l->s[i] == ' ') i++;
        if (i == l->len) return;
    }
    MdRun empty = { 0 };
    md_out(c, &empty, src);
}

/* strip the leading indent, in columns, and any trailing whitespace */
static void md_split(const MdSrc *l, int *ind, const char **body, int *blen) {
    int i = 0, col = 0;
    while (i < l->len && (l->s[i] == ' ' || l->s[i] == '\t')) {
        col += l->s[i] == '\t' ? 4 - col % 4 : 1;
        i++;
    }
    int e = l->len;
    while (e > i && (l->s[e-1] == ' ' || l->s[e-1] == '\t' || l->s[e-1] == '\r')) e--;
    *ind = col;
    *body = l->s + i;
    *blen = e - i;
}
int md_is_hr(const char *s, int n) {
    if (n < 3) return 0;
    char c = s[0];
    if (c != '-' && c != '*' && c != '_') return 0;
    int k = 0;
    for (int i = 0; i < n; i++) {
        if (s[i] == c) k++;
        else if (s[i] != ' ' && s[i] != '\t') return 0;
    }
    return k >= 3;
}
int md_is_fence(const char *s, int n, char *ch, int *fl) {
    if (n < 3 || (s[0] != '`' && s[0] != '~')) return 0;
    int k = md_run_of(s, n, 0, s[0]);
    if (k < 3) return 0;
    if (ch) *ch = s[0];
    if (fl) *fl = k;
    return 1;
}
int md_atx(const char *s, int n, int *lvl, int *ts, int *tl) {
    int k = md_run_of(s, n, 0, '#');
    if (k < 1 || k > 6) return 0;
    if (k < n && s[k] != ' ' && s[k] != '\t') return 0;
    int a = k;
    while (a < n && (s[a] == ' ' || s[a] == '\t')) a++;
    int e = n;
    while (e > a && s[e-1] == '#') e--;              /* a closing ### run */
    if (e > a && s[e-1] != ' ') e = n;
    while (e > a && s[e-1] == ' ') e--;
    *lvl = k; *ts = a; *tl = e - a;
    return 1;
}
int md_setext(const char *s, int n) {
    if (n < 1) return 0;
    if (s[0] != '=' && s[0] != '-') return 0;
    for (int i = 1; i < n; i++) if (s[i] != s[0]) return 0;
    return s[0] == '=' ? 1 : 2;
}
/* "- ", "* ", "+ ", "1. ", "2) " → marker length, and the number or -1 */
int md_bullet(const char *s, int n, int *mlen, int *ord) {
    if (n >= 2 && (s[0] == '-' || s[0] == '*' || s[0] == '+') &&
        (s[1] == ' ' || s[1] == '\t')) {
        int k = 1;
        while (k < n && (s[k] == ' ' || s[k] == '\t')) k++;
        *mlen = k; *ord = -1;
        return 1;
    }
    int d = 0;
    while (d < n && isdigit((unsigned char)s[d])) d++;
    if (d > 0 && d < n && (s[d] == '.' || s[d] == ')') &&
        d + 1 < n && (s[d+1] == ' ' || s[d+1] == '\t')) {
        int k = d + 1;
        while (k < n && (s[k] == ' ' || s[k] == '\t')) k++;
        *mlen = k;
        *ord = atoi(s);
        return 1;
    }
    return 0;
}
/* does this line start a block that a paragraph or list item can't absorb? */
static int md_starts_block(const char *s, int n) {
    int lvl, ts, tl, ml, ord;
    return n == 0 || s[0] == '>' || md_is_hr(s, n) || md_is_fence(s, n, NULL, NULL) ||
           md_atx(s, n, &lvl, &ts, &tl) || md_bullet(s, n, &ml, &ord);
}
static int md_is_tablesep(const char *s, int n) {
    int dash = 0, bar = 0;
    for (int i = 0; i < n; i++) {
        if (s[i] == '-') dash++;
        else if (s[i] == '|') bar++;
        else if (s[i] != ':' && s[i] != ' ' && s[i] != '\t') return 0;
    }
    return dash >= 1 && bar >= 1;
}
/* split a table row on unescaped pipes; returns the cell count */
static int md_cells(const char *s, int n, const char **cs, int *cl, int max) {
    int k = 0, i = 0;
    if (i < n && s[i] == '|') i++;
    int start = i;
    for (; i <= n; i++) {
        if (i < n && s[i] == '\\') { i++; continue; }
        if (i < n && s[i] != '|') continue;
        int a = start, e = i;
        while (a < e && s[a] == ' ') a++;
        while (e > a && s[e-1] == ' ') e--;
        if (i == n && a >= e && k > 0) break;        /* trailing "|" */
        if (k < max) { cs[k] = s + a; cl[k] = e - a; k++; }
        start = i + 1;
    }
    return k;
}

static void md_block(MdCtx *c, MdSrc *L, int n, int depth);

/* A table, laid out in columns that fit the pane. Cells are rendered inline
 * and then truncated rather than wrapped: a wrapped cell needs a row model
 * this viewer doesn't have, and a truncated one still lines up. */
static int md_table(MdCtx *c, MdSrc *L, int n, int at) {
    int rows = 0;
    while (at + rows < n) {
        int ind, bl; const char *b;
        md_split(&L[at + rows], &ind, &b, &bl);
        if (bl == 0 || !memchr(b, '|', (size_t)bl)) break;
        rows++;
    }
    if (rows < 2) return 0;
    int ind, bl; const char *b;
    md_split(&L[at + 1], &ind, &b, &bl);
    if (!md_is_tablesep(b, bl)) return 0;

    const char *cs[MD_MAXCOL]; int cl[MD_MAXCOL];
    md_split(&L[at + 1], &ind, &b, &bl);
    int ncol = md_cells(b, bl, cs, cl, MD_MAXCOL);
    if (ncol < 1) return 0;
    char align[MD_MAXCOL];
    for (int i = 0; i < ncol; i++) {
        int l = cl[i] > 0 && cs[i][0] == ':', r = cl[i] > 0 && cs[i][cl[i]-1] == ':';
        align[i] = (char)(l && r ? 'c' : r ? 'r' : 'l');
    }
    int nrow = rows - 1;                       /* the separator isn't a row */
    MdRun *cell = xmalloc((size_t)nrow * ncol * sizeof *cell);
    memset(cell, 0, (size_t)nrow * ncol * sizeof *cell);
    int w[MD_MAXCOL];
    for (int i = 0; i < ncol; i++) w[i] = 1;
    for (int r = 0; r < nrow; r++) {
        int sl = at + (r == 0 ? 0 : r + 1);
        md_split(&L[sl], &ind, &b, &bl);
        int k = md_cells(b, bl, cs, cl, MD_MAXCOL);
        for (int i = 0; i < ncol; i++) {
            MdRun *u = &cell[r * ncol + i];
            if (i < k) md_inline(u, cs[i], cl[i],
                                 r ? c->base : MS_ADD(c->base, MS_BOLD), 0);
            int cw = md_cols(u->s, u->n);
            if (cw > w[i]) w[i] = cw;
        }
    }
    /* shrink the widest column until the row fits, then let it overflow */
    int avail = c->width - c->pw, total;
    for (;;) {
        total = 3 * (ncol - 1);
        for (int i = 0; i < ncol; i++) total += w[i];
        if (total <= avail) break;
        int big = 0;
        for (int i = 1; i < ncol; i++) if (w[i] > w[big]) big = i;
        if (w[big] <= 4) break;
        w[big]--;
    }
    MdRun line = { 0 };
    for (int r = 0; r < nrow; r++) {
        line.n = 0;
        for (int i = 0; i < ncol; i++) {
            if (i) mr_str(&line, " │ ", MST(MC_RULE, MS_DIM));
            MdRun *u = &cell[r * ncol + i];
            int cw = md_cols(u->s, u->n);
            if (cw > w[i]) {
                mr_slice(&line, u, 0, md_byte_at(u->s, u->n, w[i] - 1));
                mr_str(&line, "…", MST(MC_META, MS_DIM));
                cw = w[i];
            } else {
                int pad = w[i] - cw, lead = align[i] == 'r' ? pad
                                          : align[i] == 'c' ? pad / 2 : 0;
                mr_rep(&line, " ", lead, c->base);
                mr_cat(&line, u);
                mr_rep(&line, " ", pad - lead, c->base);
            }
        }
        md_raw(c, &line, L[at + (r == 0 ? 0 : r + 1)].src);
        if (r == 0) {                                    /* header underline */
            line.n = 0;
            for (int i = 0; i < ncol; i++) {
                if (i) mr_str(&line, "─┼─", MST(MC_RULE, MS_DIM));
                mr_rep(&line, "─", w[i], MST(MC_RULE, MS_DIM));
            }
            md_raw(c, &line, L[at + 1].src);
        }
    }
    mr_free(&line);
    for (int i = 0; i < nrow * ncol; i++) mr_free(&cell[i]);
    free(cell);
    return rows;
}

static void md_heading(MdCtx *c, int lvl, const char *s, int n, int src) {
    MdRun t = { 0 };
    md_inline(&t, s, n, MST(MC_HEAD, MS_BOLD), 0);
    if (c->m->n) md_blank(c, src);
    md_out(c, &t, src);
    if (lvl <= 2) {                    /* the two top levels get a rule under */
        int wide = md_cols(t.s, t.n), avail = c->width - c->pw;
        if (wide > avail) wide = avail;
        MdRun r = { 0 };
        mr_rep(&r, lvl == 1 ? "═" : "─", wide, MST(MC_HEAD, MS_DIM));
        md_raw(c, &r, src);
        mr_free(&r);
    }
    mr_free(&t);
}

/* A list item: everything belonging to it is re-indented and rendered as a
 * block of its own, so an item can hold paragraphs, code or another list. */
static int md_item(MdCtx *c, MdSrc *L, int n, int at, int depth) {
    int ind, bl; const char *b;
    md_split(&L[at], &ind, &b, &bl);
    int mlen, ord;
    md_bullet(b, bl, &mlen, &ord);
    int cind = ind + mlen;

    int e = at + 1;
    while (e < n) {
        int i2, l2; const char *b2;
        md_split(&L[e], &i2, &b2, &l2);
        if (l2 == 0) {                     /* a blank only holds the item open
                                            * if indented content follows it */
            int k = e + 1;
            while (k < n) {
                int i3, l3; const char *b3;
                md_split(&L[k], &i3, &b3, &l3);
                if (l3 == 0) { k++; continue; }
                if (i3 >= cind) break;
                k = n;
            }
            if (k >= n) break;
            e++;
            continue;
        }
        if (i2 >= cind) { e++; continue; }
        if (md_starts_block(b2, l2)) break;
        e++;                               /* lazy continuation of the text */
    }

    int cnt = e - at;
    MdSrc *sub = xmalloc((size_t)cnt * sizeof *sub);
    sub[0].s = b + mlen; sub[0].len = bl - mlen; sub[0].src = L[at].src;
    for (int i = 1; i < cnt; i++) {
        int i2, l2; const char *b2;
        md_split(&L[at + i], &i2, &b2, &l2);
        /* dedent by the item's content indent, no further */
        int drop = i2 < cind ? i2 : cind;
        const char *p = L[at + i].s;
        int col = 0, k = 0;
        while (k < L[at + i].len && col < drop &&
               (p[k] == ' ' || p[k] == '\t')) {
            col += p[k] == '\t' ? 4 - col % 4 : 1;
            k++;
        }
        int en = L[at + i].len;
        while (en > k && (p[en-1] == ' ' || p[en-1] == '\r')) en--;
        sub[i].s = p + k; sub[i].len = en - k; sub[i].src = L[at + i].src;
    }

    char mark[16];
    if (ord >= 0) snprintf(mark, sizeof mark, "%d. ", ord);
    else {
        int d = c->ldepth;
        snprintf(mark, sizeof mark, "%s", d == 0 ? "• " : d == 1 ? "◦ " : "▪ ");
    }
    /* a task list reads better as a box than as literal brackets */
    if (sub[0].len >= 3 && sub[0].s[0] == '[' && sub[0].s[2] == ']' &&
        (sub[0].len == 3 || sub[0].s[3] == ' ')) {
        char t = sub[0].s[1];
        if (t == ' ' || t == 'x' || t == 'X') {
            snprintf(mark, sizeof mark, "%s ", t == ' ' ? "[ ]" : "[x]");
            int cut = sub[0].len > 3 ? 4 : 3;
            sub[0].s += cut; sub[0].len -= cut;
        }
    }
    char pad[16];
    snprintf(pad, sizeof pad, "%*s", md_cols(mark, (int)strlen(mark)), "");
    MdCtx ic;
    md_ctx_child(&ic, c, mark, pad, MST(MC_MARK, 0));
    ic.ldepth = c->ldepth + 1;
    md_block(&ic, sub, cnt, depth + 1);
    md_ctx_free(&ic);
    free(sub);
    return cnt;
}

static void md_block(MdCtx *c, MdSrc *L, int n, int depth) {
    MdRun para = { 0 };
    for (int i = 0; i < n; ) {
        int ind, bl; const char *b;
        md_split(&L[i], &ind, &b, &bl);
        int src = L[i].src;

        if (bl == 0) { md_blank(c, src); i++; continue; }

        char fc; int fl;
        if (ind < 4 && md_is_fence(b, bl, &fc, &fl)) {      /* fenced code */
            int e = i + 1;
            while (e < n) {
                int i2, l2; const char *b2;
                md_split(&L[e], &i2, &b2, &l2);
                char c2; int l3;
                if (md_is_fence(b2, l2, &c2, &l3) && c2 == fc && l3 >= fl) break;
                e++;
            }
            MdRun r = { 0 };
            for (int k = i + 1; k < e; k++) {
                r.n = 0;
                int drop = ind, col = 0, j = 0;
                const char *p = L[k].s;
                while (j < L[k].len && col < drop && (p[j] == ' ' || p[j] == '\t')) {
                    col += p[j] == '\t' ? 4 - col % 4 : 1;
                    j++;
                }
                mr_str(&r, "  ", MST(MC_CODE, 0));
                mr_add(&r, p + j, L[k].len - j, MST(MC_CODE, 0));
                md_raw(c, &r, L[k].src);
            }
            mr_free(&r);
            i = e < n ? e + 1 : e;
            continue;
        }
        if (ind >= 4 && depth == 0) {                     /* indented code */
            int e = i;
            while (e < n) {
                int i2, l2; const char *b2;
                md_split(&L[e], &i2, &b2, &l2);
                if (l2 == 0) {                 /* blanks only if code resumes */
                    int k = e + 1;
                    while (k < n) {
                        int i3, l3; const char *b3;
                        md_split(&L[k], &i3, &b3, &l3);
                        if (l3 == 0) { k++; continue; }
                        if (i3 >= 4) break;
                        k = n;
                    }
                    if (k >= n) break;
                    e++;
                    continue;
                }
                if (i2 < 4) break;
                e++;
            }
            MdRun r = { 0 };
            for (int k = i; k < e; k++) {
                int i2, l2; const char *b2;
                md_split(&L[k], &i2, &b2, &l2);
                r.n = 0;
                mr_str(&r, "  ", MST(MC_CODE, 0));
                mr_rep(&r, " ", i2 - 4, MST(MC_CODE, 0));
                mr_add(&r, b2, l2, MST(MC_CODE, 0));
                md_raw(c, &r, L[k].src);
            }
            mr_free(&r);
            i = e;
            continue;
        }
        int lvl, ts, tl;
        if (md_atx(b, bl, &lvl, &ts, &tl)) {                    /* # heading */
            md_heading(c, lvl, b + ts, tl, src);
            i++;
            continue;
        }
        if (md_is_hr(b, bl)) {                             /* thematic break */
            int avail = c->width - c->pw;
            MdRun r = { 0 };
            mr_rep(&r, "─", avail > 0 ? avail : 1, MST(MC_RULE, MS_DIM));
            md_raw(c, &r, src);
            mr_free(&r);
            i++;
            continue;
        }
        if (b[0] == '>') {                                    /* blockquote */
            int e = i;
            while (e < n) {
                int i2, l2; const char *b2;
                md_split(&L[e], &i2, &b2, &l2);
                if (l2 == 0) break;
                if (b2[0] != '>' && (e == i || md_starts_block(b2, l2))) break;
                e++;
            }
            int cnt = e - i;
            MdSrc *sub = xmalloc((size_t)cnt * sizeof *sub);
            for (int k = 0; k < cnt; k++) {
                int i2, l2; const char *b2;
                md_split(&L[i + k], &i2, &b2, &l2);
                if (l2 && b2[0] == '>') {
                    b2++; l2--;
                    if (l2 && b2[0] == ' ') { b2++; l2--; }
                }
                sub[k].s = b2; sub[k].len = l2; sub[k].src = L[i + k].src;
            }
            MdCtx qc;
            md_ctx_child(&qc, c, "│ ", "│ ", MST(MC_RULE, MS_DIM));
            qc.base = MS_COL(c->base, MC_QUOTE);
            if (depth < MD_MAXDEPTH) md_block(&qc, sub, cnt, depth + 1);
            md_ctx_free(&qc);
            free(sub);
            i = e;
            continue;
        }
        int mlen, ord;
        if (md_bullet(b, bl, &mlen, &ord) && depth < MD_MAXDEPTH) {
            i += md_item(c, L, n, i, depth);
            continue;
        }
        if (memchr(b, '|', (size_t)bl)) {                          /* table */
            int used = md_table(c, L, n, i);
            if (used) { i += used; continue; }
        }
        /* paragraph: run on until a blank line or a new block, honouring the
         * two-space hard break and a "===" underline turning it into a head */
        para.n = 0;
        int e = i;
        while (e < n) {
            int i2, l2; const char *b2;
            md_split(&L[e], &i2, &b2, &l2);
            if (l2 == 0) break;
            if (e > i) {
                int st = md_setext(b2, l2);
                if (st) {
                    md_heading(c, st, para.s, para.n, src);
                    para.n = 0;
                    e++;
                    break;
                }
                if (md_starts_block(b2, l2) || md_is_tablesep(b2, l2)) break;
            }
            if (para.n) mr_add(&para, " ", 1, c->base);
            int hard = L[e].len >= 2 && L[e].s[L[e].len-1] == ' ' &&
                       L[e].s[L[e].len-2] == ' ';
            if (l2 && b2[l2-1] == '\\') { l2--; hard = 1; }
            md_inline(&para, b2, l2, c->base, 0);
            e++;
            if (hard) { md_out(c, &para, L[e-1].src); para.n = 0; }
        }
        if (para.n) md_out(c, &para, src);
        i = e > i ? e : i + 1;
    }
    mr_free(&para);
}

/* Rebuild `b`'s rendered view for a pane `width` columns wide. */
void md_render(Buf *b, int width) {
    if (!b->md) b->md = calloc(1, sizeof *b->md);
    Md *m = b->md;
    if (!m) die("out of memory");
    md_clear(m);
    m->laid_w = width;
    m->ver = b->ver;
    if (width < 12) width = 12;
    if (cfg_md_width > 0 && width > cfg_md_width) width = cfg_md_width;

    MdSrc *L = xmalloc((size_t)b->n * sizeof *L);
    int n = 0;
    for (int i = 0; i < b->n; i++) {
        L[n].s = b->ln[i].s;
        L[n].len = b->ln[i].len;
        L[n].src = i;
        n++;
    }
    MdCtx c;
    memset(&c, 0, sizeof c);
    c.m = m;
    c.width = width;

    int at = 0;
    /* YAML front matter is metadata, not prose — show it as such */
    if (n > 1 && L[0].len == 3 && memcmp(L[0].s, "---", 3) == 0) {
        int e = 1;
        while (e < n && !(L[e].len == 3 &&
                          (memcmp(L[e].s, "---", 3) == 0 ||
                           memcmp(L[e].s, "...", 3) == 0))) e++;
        if (e < n) {
            MdRun r = { 0 };
            for (int k = 1; k < e; k++) {
                r.n = 0;
                mr_add(&r, L[k].s, L[k].len, MST(MC_META, MS_DIM));
                md_raw(&c, &r, L[k].src);
            }
            mr_free(&r);
            at = e + 1;
        }
    }
    md_block(&c, L + at, n - at, 0);
    free(L);
    md_ctx_free(&c);
    if (m->n == 0) md_push(m, "", (const unsigned char *)"", 0, 0);
    if (m->rowoff > m->n - 1) m->rowoff = m->n - 1;
    if (m->rowoff < 0) m->rowoff = 0;
}
/* The render is only valid for the width and the buffer version it was made
 * from; anything else and it is built again. */
Md *md_view_of(Buf *b, int width) {
    if (!b->md || b->md->laid_w != width || b->md->ver != b->ver)
        md_render(b, width);
    return b->md;
}
/* Rendered line showing source line `sy` — used to keep the reading position
 * when the view is toggled. */
int md_row_for_src(Md *m, int sy) {
    for (int i = 0; i < m->n; i++)
        if (m->src[i] >= sy) return i;         /* the top of that block */
    return m->n ? m->n - 1 : 0;
}
int md_is_md(Buf *b) {
    return b->kind == TAB_FILE && b->lang && b->lang->md;
}
