#include "core/sds.h"
#include "editor/editor.h"
#include "editor/editor_internal.h"
#include "syntax/syntax.h"
#include "pdf/pdf.h"
#include "markdown/markdown.h"
#include "terminal/terminal.h"
#include "lsp/lsp.h"

/* ── line ops ─────────────────────────────────────────────────────── */
static void line_grow(Line *l, int need) {
    if (l->cap >= need) return;
    l->cap = need < 32 ? 32 : need * 2;
    l->s = xrealloc(l->s, (size_t)l->cap);
}
/* ── buffer core ──────────────────────────────────────────────────── */
void buf_insert_line(Buf *b, int at, const char *s, int len) {
    if (b->n == b->cap) {
        b->cap = b->cap ? b->cap * 2 : 64;
        b->ln = xrealloc(b->ln, (size_t)b->cap * sizeof(Line));
    }
    memmove(b->ln + at + 1, b->ln + at, (size_t)(b->n - at) * sizeof(Line));
    Line *l = &b->ln[at];
    l->len = len; l->cap = 0; l->s = NULL; l->hst = 0;
    line_grow(l, len ? len : 1);
    memcpy(l->s, s, (size_t)len);
    b->n++;
}
static void buf_del_line(Buf *b, int at) {
    free(b->ln[at].s);
    memmove(b->ln + at, b->ln + at + 1, (size_t)(b->n - at - 1) * sizeof(Line));
    b->n--;
}

void urec_free(URec *r) { free(r->t); }
void buf_free(Buf *b) {
    lsp_detach(b);
    for (int i = 0; i < b->n; i++) free(b->ln[i].s);
    for (int i = 0; i < b->nundo; i++) urec_free(&b->undo[i]);
    for (int i = 0; i < b->nredo; i++) urec_free(&b->redo[i]);
    free(b->undo); free(b->redo);
    free(b->ln);
    if (b->term) term_free(b->term);
    if (b->pdf)  pdf_free(b->pdf);
    if (b->md)   md_free(b->md);
#ifdef SDS_TREESITTER
    ts_forget(b);
    if (b->ts_tree)   ts_tree_delete(b->ts_tree);
    if (b->ts_parser) ts_parser_delete(b->ts_parser);
    free(b->ts_off);
#endif
    free(b);
}
Buf *buf_load(const char *path) {
    if (!path || !*path) return NULL;   /* also tells gcc -O1 it's non-null */
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    Buf *b = calloc(1, sizeof *b);
    if (!b) die("out of memory");
    snprintf(b->path, sizeof b->path, "%s", path);
    const char *slash = strrchr(path, '/');
    snprintf(b->name, sizeof b->name, "%s", slash ? slash + 1 : path);
    b->lang = lang_for(path);
    char *ln = NULL; size_t cap = 0; ssize_t r;
    while ((r = getline(&ln, &cap, f)) != -1) {
        while (r > 0 && (ln[r-1] == '\n' || ln[r-1] == '\r')) r--;
        buf_insert_line(b, b->n, ln, (int)r);
    }
    free(ln); fclose(f);
    if (b->n == 0) buf_insert_line(b, 0, "", 0);
    return b;
}
int buf_save(Buf *b) {
    FILE *f = fopen(b->path, "w");
    if (!f) return -1;
    for (int i = 0; i < b->n; i++) {
        fwrite(b->ln[i].s, 1, (size_t)b->ln[i].len, f);
        fputc('\n', f);
    }
    fclose(f);
    b->dirty = 0;
    return 0;
}

/* ── raw edit primitives (no undo recording) ──────────────────────── */
/* Called from every edit path, so this is where the buffer version — and with
 * it the tree-sitter reparse and span cache — gets invalidated. */
void hl_invalidate(Buf *b, int y) {
    if (y < b->hl_upto) b->hl_upto = y;
    if (y > b->hl_dirty) b->hl_dirty = y;
    b->ver++;
#ifdef SDS_TREESITTER
    b->ts_off_dirty = 1;
#endif
}

#ifdef SDS_TREESITTER

/* Byte offset of (y,x). Callers must use it before mutating the buffer —
 * the offset table describes the text as it currently stands. */
static uint32_t ts_byte_at(Buf *b, int y, int x) {
    ts_offsets(b);
    if (y < 0) return 0;
    if (y >= b->n) return b->ts_bytes;
    return b->ts_off[y] + (uint32_t)x;
}
/* Tell tree-sitter what changed so the next parse can reuse the old tree.
 * Without this a full reparse runs on every keystroke, which measured ~88ms
 * on a 3.7k-line file; with it, parsing is proportional to the edit. */
static void ts_note_edit(Buf *b, uint32_t sb, uint32_t ob, uint32_t nb,
                         int sy, int sx, int oy, int ox, int ny, int nx) {
    b->ts_off_dirty = 1;
    ts_shift_spans(b, sb, ob, nb);
    if (!b->ts_tree) return;
    TSInputEdit e;
    e.start_byte   = sb;
    e.old_end_byte = ob;
    e.new_end_byte = nb;
    e.start_point    = (TSPoint){ (uint32_t)sy, (uint32_t)sx };
    e.old_end_point  = (TSPoint){ (uint32_t)oy, (uint32_t)ox };
    e.new_end_point  = (TSPoint){ (uint32_t)ny, (uint32_t)nx };
    ts_tree_edit(b->ts_tree, &e);
}
#endif

void ins_text(Buf *b, int y, int x, const char *t, int len,
                     int *ey, int *ex) {
    int y0 = y, x0 = x;
#ifdef SDS_TREESITTER
    int sy = y, sx = x;
    uint32_t sb = ts_byte_at(b, y, x);
#endif
    int i = 0;
    while (i < len) {
        int j = i;
        while (j < len && t[j] != '\n') j++;
        int seg = j - i;
        Line *l = &b->ln[y];
        line_grow(l, l->len + seg);
        memmove(l->s + x + seg, l->s + x, (size_t)(l->len - x));
        memcpy(l->s + x, t + i, (size_t)seg);
        l->len += seg;
        x += seg;
        if (j < len) {                    /* newline: split */
            l = &b->ln[y];
            buf_insert_line(b, y + 1, l->s + x, l->len - x);
            b->ln[y].len = x;
            y++; x = 0;
        }
        i = j + 1;
    }
#ifdef SDS_TREESITTER
    ts_note_edit(b, sb, sb, sb + (uint32_t)len, sy, sx, sy, sx, y, x);
#endif
    /* text at the start of a line pushes that line down; anywhere else the
     * line keeps its number and only what follows the split moves */
    if (y > y0) lsp_lines_inserted(b, x0 == 0 ? y0 : y0 + 1, y - y0);
    /* the damage reaches the last line the text landed on; lines that moved
     * carry their start state with them, but only up to where a pass had
     * actually been */
    if (y > b->hl_dirty) b->hl_dirty = y;
    if (y > y0 && b->hl_seen > y0) b->hl_seen = y0;
    if (ey) *ey = y;
    if (ex) *ex = x;
}
/* extract text of a (normalized) range into a malloc'd string */
char *range_text(Buf *b, int y1, int x1, int y2, int x2, int *outlen) {
    size_t cap = 64, n = 0;
    char *t = xmalloc(cap);
    for (int y = y1; y <= y2; y++) {
        Line *l = &b->ln[y];
        int a = (y == y1) ? x1 : 0;
        int z = (y == y2) ? x2 : l->len;
        size_t need = n + (size_t)(z - a) + 2;
        if (need > cap) { cap = need * 2; t = xrealloc(t, cap); }
        memcpy(t + n, l->s + a, (size_t)(z - a));
        n += (size_t)(z - a);
        if (y < y2) t[n++] = '\n';
    }
    t[n] = 0;
    if (outlen) *outlen = (int)n;
    return t;
}
void del_range_raw(Buf *b, int y1, int x1, int y2, int x2) {
#ifdef SDS_TREESITTER
    uint32_t sb = ts_byte_at(b, y1, x1), ob = ts_byte_at(b, y2, x2);
    ts_note_edit(b, sb, ob, sb, y1, x1, y2, x2, y1, x1);
#endif
    if (y1 == y2) {
        Line *l = &b->ln[y1];
        memmove(l->s + x1, l->s + x2, (size_t)(l->len - x2));
        l->len -= x2 - x1;
    } else {
        Line *a = &b->ln[y1], *z = &b->ln[y2];
        line_grow(a, x1 + (z->len - x2));
        memcpy(a->s + x1, z->s + x2, (size_t)(z->len - x2));
        a->len = x1 + (z->len - x2);
        for (int y = y2; y > y1; y--) buf_del_line(b, y);
        lsp_lines_deleted(b, y1, x1, y2, x2);
        if (b->hl_seen > y1) b->hl_seen = y1;
    }
}
void text_end(int y, int x, const char *t, int len, int *ey, int *ex) {
    for (int i = 0; i < len; i++) {
        if (t[i] == '\n') { y++; x = 0; }
        else x++;
    }
    *ey = y; *ex = x;
}

/* ── selection ────────────────────────────────────────────────────── */
int sel_norm(Buf *b, int *y1, int *x1, int *y2, int *x2) {
    if (!b->sel || (b->ay == b->cy && b->ax == b->cx)) return 0;
    if (b->ay < b->cy || (b->ay == b->cy && b->ax < b->cx)) {
        *y1 = b->ay; *x1 = b->ax; *y2 = b->cy; *x2 = b->cx;
    } else {
        *y1 = b->cy; *x1 = b->cx; *y2 = b->ay; *x2 = b->ax;
    }
    return 1;
}
void sel_delete(Buf *b) {             /* assumes active selection */
    int y1, x1, y2, x2;
    if (sel_norm(b, &y1, &x1, &y2, &x2)) edit_del(b, y1, x1, y2, x2);
    b->sel = 0;
}
