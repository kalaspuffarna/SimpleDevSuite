#include "core/sds.h"
#include "editor/editor.h"
#include "editor/editor_internal.h"

static int   g_group = 0;      /* undo group counter */
int   g_lastkind = 0;   /* for coalescing typed runs */

/* ── undo ─────────────────────────────────────────────────────────── */
static void redo_clear(Buf *b) {
    for (int i = 0; i < b->nredo; i++) urec_free(&b->redo[i]);
    b->nredo = 0;
}
static void push_undo(Buf *b, int type, int y, int x, char *t, int tlen) {
    if (b->nundo >= UNDO_MAX) {           /* drop the oldest group */
        int g = b->undo[0].group, k = 0;
        while (k < b->nundo && b->undo[k].group == g) urec_free(&b->undo[k++]);
        memmove(b->undo, b->undo + k, (size_t)(b->nundo - k) * sizeof(URec));
        b->nundo -= k;
    }
    if (b->nundo == b->undocap) {      /* doubling: this runs per keystroke */
        b->undocap = b->undocap ? b->undocap * 2 : 64;
        b->undo = xrealloc(b->undo, (size_t)b->undocap * sizeof(URec));
    }
    URec *r = &b->undo[b->nundo++];
    r->type = type; r->y = y; r->x = x; r->t = t; r->tlen = tlen;
    r->group = g_group; r->cy = b->cy; r->cx = b->cx;
}
void begin_action(int kind) {
    /* coalesce runs of plain typing / plain backspacing into one group */
    if (!(kind != AK_OTHER && kind == g_lastkind)) g_group++;
    g_lastkind = kind;
}
/* recorded edits — all user-visible modifications go through these */
void edit_ins(Buf *b, int y, int x, const char *t, int len) {
    push_undo(b, U_INS, y, x, len ? memcpy(xmalloc((size_t)len + 1), t, (size_t)len) : xstrdup(""), len);
    if (len) b->undo[b->nundo - 1].t[len] = 0;
    int ey, ex;
    ins_text(b, y, x, t, len, &ey, &ex);
    b->cy = ey; b->cx = ex;
    b->dirty = 1;
    hl_invalidate(b, y);
    redo_clear(b);
}
void edit_del(Buf *b, int y1, int x1, int y2, int x2) {
    if (y1 > y2 || (y1 == y2 && x1 > x2)) {
        int ty = y1, tx = x1; y1 = y2; x1 = x2; y2 = ty; x2 = tx;
    }
    int tlen;
    char *t = range_text(b, y1, x1, y2, x2, &tlen);
    push_undo(b, U_DEL, y1, x1, t, tlen);
    del_range_raw(b, y1, x1, y2, x2);
    b->cy = y1; b->cx = x1;
    b->dirty = 1;
    hl_invalidate(b, y1);
    redo_clear(b);
}
void do_undo(Buf *b) {
    if (!b->nundo) { set_msg("nothing to undo", NULL); return; }
    int g = b->undo[b->nundo - 1].group;
    int rcy = 0, rcx = 0;
    while (b->nundo && b->undo[b->nundo - 1].group == g) {
        URec r = b->undo[--b->nundo];
        if (r.type == U_INS) {
            int ey, ex;
            text_end(r.y, r.x, r.t, r.tlen, &ey, &ex);
            del_range_raw(b, r.y, r.x, ey, ex);
        } else {
            ins_text(b, r.y, r.x, r.t, r.tlen, NULL, NULL);
        }
        hl_invalidate(b, r.y);
        rcy = r.cy; rcx = r.cx;
        if (b->nredo == b->redocap) {
            b->redocap = b->redocap ? b->redocap * 2 : 64;
            b->redo = xrealloc(b->redo, (size_t)b->redocap * sizeof(URec));
        }
        b->redo[b->nredo++] = r;
    }
    b->cy = min2(rcy, b->n - 1);
    b->cx = min2(rcx, b->ln[b->cy].len);
    b->sel = 0; b->dirty = 1;
    g_lastkind = AK_OTHER;
}
void do_redo(Buf *b) {
    if (!b->nredo) { set_msg("nothing to redo", NULL); return; }
    int g = b->redo[b->nredo - 1].group;
    while (b->nredo && b->redo[b->nredo - 1].group == g) {
        URec r = b->redo[--b->nredo];
        if (r.type == U_INS) {
            int ey, ex;
            ins_text(b, r.y, r.x, r.t, r.tlen, &ey, &ex);
            b->cy = ey; b->cx = ex;
        } else {
            int ey, ex;
            text_end(r.y, r.x, r.t, r.tlen, &ey, &ex);
            del_range_raw(b, r.y, r.x, ey, ex);
            b->cy = r.y; b->cx = r.x;
        }
        hl_invalidate(b, r.y);
        if (b->nundo == b->undocap) {
            b->undocap = b->undocap ? b->undocap * 2 : 64;
            b->undo = xrealloc(b->undo, (size_t)b->undocap * sizeof(URec));
        }
        b->undo[b->nundo++] = r;
    }
    b->sel = 0; b->dirty = 1;
    g_lastkind = AK_OTHER;
}
