#include "core/sds.h"
#include "lsp/lsp.h"
#include "lsp/lsp_internal.h"

/* ── documents ────────────────────────────────────────────────────────
 * A buffer the server knows about. The whole text goes over on open and
 * again, debounced, after edits (full-document sync: simple, and cheap next
 * to the parse the server does on every change anyway). What comes back is
 * a list of diagnostics, kept sorted by line so the renderer can find a
 * line's messages with a binary search.                                   */

int  cfg_lsp_enabled = 1;
char cfg_clangd[256] = "clangd";

LspDoc *lsp_docs[MAX_TABS];
int     lsp_ndocs = 0;

/* Which languages have a server, and under what id the protocol knows them. */
static const struct { const char *lang, *id; char *cmd; } servers[] = {
    { "c",   "c",   cfg_clangd },
    { "c++", "cpp", cfg_clangd },
};

/* file:// URI for a path, percent-encoding everything outside the unreserved
 * set so a space or '#' in a directory name survives. */
char *lsp_uri(const char *path) {
    static const char hex[] = "0123456789ABCDEF";
    char abs[PATH_MAX];
    if (!realpath(path, abs)) snprintf(abs, sizeof abs, "%s", path);
    size_t n = strlen(abs);
    char *u = xmalloc(n * 3 + 8), *o = u;
    memcpy(o, "file://", 7);
    o += 7;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)abs[i];
        if (isalnum(c) || strchr("/-._~", c)) *o++ = (char)c;
        else { *o++ = '%'; *o++ = hex[c >> 4]; *o++ = hex[c & 15]; }
    }
    *o = 0;
    return u;
}
static LspDoc *doc_of(Buf *b) {
    return b ? b->lsp : NULL;
}
static void diags_free(LspDoc *d) {
    for (int i = 0; i < d->nd; i++) free(d->d[i].msg);
    free(d->d);
    d->d = NULL;
    d->nd = 0;
}

/* Start tracking `b` if its language has a server. Safe to call on any
 * buffer; the ones without a server are simply left alone. */
void lsp_attach(Buf *b) {
    if (!cfg_lsp_enabled || !b || b->lsp || b->kind != TAB_FILE || !b->lang) return;
    if (lsp_ndocs == MAX_TABS) return;
    int k = -1;
    for (size_t i = 0; i < sizeof servers / sizeof *servers; i++)
        if (!strcmp(servers[i].lang, b->lang->name)) { k = (int)i; break; }
    if (k < 0 || !servers[k].cmd[0]) return;
    LspServer *s = lsp_server_for(servers[k].cmd);
    if (!s || lsp_server_dead(s)) return;

    LspDoc *d = calloc(1, sizeof *d);
    if (!d) die("out of memory");
    d->b = b;
    d->srv = s;
    d->uri = lsp_uri(b->path);
    d->lang_id = servers[k].id;
    d->ver_seen = b->ver;
    d->seen_ms = lsp_now_ms();
    b->lsp = d;
    lsp_docs[lsp_ndocs++] = d;
    if (lsp_server_ready(s)) lsp_send_open(d);
    /* otherwise the initialize reply opens it */
}
void lsp_detach(Buf *b) {
    LspDoc *d = doc_of(b);
    if (!d) return;
    if (d->opened && !lsp_server_dead(d->srv)) {
        JBuf p = { 0 };
        jb_cat(&p, "{\"textDocument\":{\"uri\":");
        jb_str(&p, d->uri, strlen(d->uri));
        jb_cat(&p, "}}");
        lsp_notify(d->srv, "textDocument/didClose", &p);
        free(p.s);
    }
    for (int i = 0; i < lsp_ndocs; i++)
        if (lsp_docs[i] == d) {
            memmove(lsp_docs + i, lsp_docs + i + 1,
                    (size_t)(lsp_ndocs - i - 1) * sizeof *lsp_docs);
            lsp_ndocs--;
            break;
        }
    diags_free(d);
    free(d->uri);
    free(d);
    b->lsp = NULL;
}
/* The buffer as the file would be saved: every line followed by a newline. */
static void text_of(JBuf *p, Buf *b) {
    JBuf t = { 0 };
    for (int i = 0; i < b->n; i++) {
        jb_raw(&t, b->ln[i].s, (size_t)b->ln[i].len);
        jb_raw(&t, "\n", 1);
    }
    jb_str(p, t.s ? t.s : "", t.n);
    free(t.s);
}
void lsp_send_open(LspDoc *d) {
    JBuf p = { 0 };
    jb_cat(&p, "{\"textDocument\":{\"uri\":");
    jb_str(&p, d->uri, strlen(d->uri));
    jb_cat(&p, ",\"languageId\":\"");
    jb_cat(&p, d->lang_id);
    jb_cat(&p, "\",\"version\":");
    jb_int(&p, d->b->ver);
    jb_cat(&p, ",\"text\":");
    text_of(&p, d->b);
    jb_cat(&p, "}}");
    lsp_notify(d->srv, "textDocument/didOpen", &p);
    free(p.s);
    d->opened = 1;
    d->ver_sent = d->b->ver;
    d->fresh = 0;
}
void lsp_send_change(LspDoc *d) {
    JBuf p = { 0 };
    jb_cat(&p, "{\"textDocument\":{\"uri\":");
    jb_str(&p, d->uri, strlen(d->uri));
    jb_cat(&p, ",\"version\":");
    jb_int(&p, d->b->ver);
    jb_cat(&p, "},\"contentChanges\":[{\"text\":");
    text_of(&p, d->b);
    jb_cat(&p, "}]}");
    lsp_notify(d->srv, "textDocument/didChange", &p);
    free(p.s);
    d->ver_sent = d->b->ver;
    d->fresh = 0;
}

/* Positions on the wire count UTF-16 code units; the buffer counts bytes. */
static int byte_col(Buf *b, int line, long u16) {
    if (line < 0 || line >= b->n) return 0;
    Line *l = &b->ln[line];
    int i = 0;
    long units = 0;
    while (i < l->len && units < u16) {
        unsigned char c = (unsigned char)l->s[i];
        int len = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
        units += len == 4 ? 2 : 1;            /* astral planes take a pair */
        i += len;
    }
    return i < l->len ? i : l->len;
}
static int diag_cmp(const void *x, const void *y) {
    const Diag *a = x, *b = y;
    if (a->line != b->line) return a->line < b->line ? -1 : 1;
    if (a->severity != b->severity) return a->severity - b->severity;
    return a->col - b->col;
}
void lsp_on_diagnostics(JSpan params) {
    char *uri = js_str(js_get(params, "uri"));
    if (!uri) return;
    LspDoc *d = NULL;
    for (int i = 0; i < lsp_ndocs; i++)
        if (!strcmp(lsp_docs[i]->uri, uri)) { d = lsp_docs[i]; break; }
    free(uri);
    if (!d) return;
    /* Only a set computed for the text the buffer still has is placed
     * exactly. While typing continues the old set stays, kept in step by the
     * line hooks below, and the next publish replaces it. */
    long ver = js_int(js_get(params, "version"), -1);
    if ((ver >= 0 && ver != d->ver_sent) || d->b->ver != d->ver_sent) return;

    diags_free(d);
    JSpan arr = js_get(params, "diagnostics");
    const char *p = js_open(arr, '[');
    JSpan item;
    int cap = 0;
    while (p && js_next(&p, arr.e, &item)) {
        JSpan start = js_get(js_get(item, "range"), "start");
        int line = (int)js_int(js_get(start, "line"), -1);
        if (line < 0 || line >= d->b->n) continue;
        char *msg = js_str(js_get(item, "message"));
        if (!msg) continue;
        if (d->nd == cap) {
            cap = cap ? cap * 2 : 16;
            d->d = xrealloc(d->d, (size_t)cap * sizeof *d->d);
        }
        Diag *g = &d->d[d->nd++];
        g->line = line;
        g->col = byte_col(d->b, line, js_int(js_get(start, "character"), 0));
        g->severity = (int)js_int(js_get(item, "severity"), DIAG_ERROR);
        if (g->severity < DIAG_ERROR || g->severity > DIAG_HINT) g->severity = DIAG_ERROR;
        g->msg = msg;
    }
    qsort(d->d, (size_t)d->nd, sizeof *d->d, diag_cmp);
    d->fresh = 1;
}

/* The diagnostics on line `li`: sets *first and returns how many. */
int lsp_line_diags(Buf *b, int li, const Diag **first) {
    LspDoc *d = doc_of(b);
    if (!d || !d->nd) return 0;
    int lo = 0, hi = d->nd;
    while (lo < hi) {                          /* first entry with line >= li */
        int mid = (lo + hi) / 2;
        if (d->d[mid].line < li) lo = mid + 1; else hi = mid;
    }
    int n = 0;
    while (lo + n < d->nd && d->d[lo + n].line == li) n++;
    if (n && first) *first = &d->d[lo];
    return n;
}
/* -1: no server for this buffer (none configured, not installed, or it
 * stopped); 0: waiting for diagnostics on the current text; 1: they are in. */
int lsp_doc_state(Buf *b) {
    LspDoc *d = doc_of(b);
    if (!d || lsp_server_dead(d->srv)) return -1;
    return d->fresh && d->ver_sent == b->ver;
}

/* ── keeping positions in step with edits ─────────────────────────────
 * A new set of diagnostics is a debounce and a re-parse away. Until then the
 * old ones follow the text they point at, so pressing Enter above an error
 * does not leave its message hanging under the wrong line.                 */

/* `n` lines appeared, so everything from line `at` on moves down. */
void lsp_lines_inserted(Buf *b, int at, int n) {
    LspDoc *d = doc_of(b);
    if (!d || n <= 0) return;
    for (int i = 0; i < d->nd; i++)
        if (d->d[i].line >= at) d->d[i].line += n;
}
/* Text from (y1,x1) to (y2,x2) went away; line y2's tail joined line y1. */
void lsp_lines_deleted(Buf *b, int y1, int x1, int y2, int x2) {
    LspDoc *d = doc_of(b);
    if (!d || y2 <= y1) return;
    int whole = (x1 == 0 && x2 == 0);          /* removed lines y1..y2-1 outright */
    int k = 0;
    for (int i = 0; i < d->nd; i++) {
        Diag g = d->d[i];
        if (g.line > y2) g.line -= y2 - y1;
        else if (g.line == y2) g.line = y1;
        else if (g.line > y1 || (g.line == y1 && whole)) { free(g.msg); continue; }
        d->d[k++] = g;
    }
    d->nd = k;
    qsort(d->d, (size_t)d->nd, sizeof *d->d, diag_cmp);
}
