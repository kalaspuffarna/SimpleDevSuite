#include "core/sds.h"
#include "editor/editor.h"
#include "editor/editor_internal.h"
#include "input/input.h"
#include "ui/ui.h"

/* ── editing ──────────────────────────────────────────────────────── */
static const char *indent_unit(Buf *b) {
    return b->lang->soft_tabs ? "    " : "\t";
}
static int line_indent_len(Line *l) {
    int i = 0;
    while (i < l->len && (l->s[i] == ' ' || l->s[i] == '\t')) i++;
    return i;
}
void ed_type(Buf *b, int c) {
    begin_action(AK_TYPE);
    if (b->sel) { sel_delete(b); g_lastkind = AK_OTHER; }
    Line *l = &b->ln[b->cy];
    char ch = (char)c;
    /* skip over an identical auto-closable closing char */
    if (strchr(")]}\"'`", c) && b->cx < l->len && l->s[b->cx] == ch) {
        b->cx++;
        return;
    }
    /* dedent a lone '}' — eat one tab or up to tabstop spaces */
    if (c == '}' && line_indent_len(l) == b->cx && b->cx > 0) {
        int cut = 0;
        if (l->s[b->cx - 1] == '\t') cut = 1;
        else while (cut < tabstop && cut < b->cx && l->s[b->cx - 1 - cut] == ' ')
            cut++;
        if (cut) edit_del(b, b->cy, b->cx - cut, b->cy, b->cx);
    }
    /* auto-close pairs */
    const char *opens = "([{", *closes = ")]}";
    const char *p = strchr(opens, c);
    l = &b->ln[b->cy];
    int nextc = b->cx < l->len ? l->s[b->cx] : 0;
    int prevc = b->cx > 0 ? l->s[b->cx - 1] : 0;
    if (p && (!nextc || strchr(" \t)]}", nextc))) {
        char pair[3] = { ch, closes[p - opens], 0 };
        edit_ins(b, b->cy, b->cx, pair, 2);
        b->cx--;
        return;
    }
    if ((c == '"' || c == '\'' || c == '`') &&
        (!nextc || strchr(" \t)]}", nextc)) && !word_ch(prevc)) {
        char pair[3] = { ch, ch, 0 };
        edit_ins(b, b->cy, b->cx, pair, 2);
        b->cx--;
        return;
    }
    edit_ins(b, b->cy, b->cx, &ch, 1);
}
void ed_enter(Buf *b) {
    begin_action(AK_OTHER);
    if (b->sel) sel_delete(b);
    Line *l = &b->ln[b->cy];
    int ind = min2(line_indent_len(l), b->cx);
    char prev = b->cx > 0 ? l->s[b->cx - 1] : 0;
    char next = b->cx < l->len ? l->s[b->cx] : 0;
    int deeper = prev && (strchr("([{", prev) ||
                          (b->lang->t1[0] /*python-ish*/ && prev == ':'));
    const char *u = indent_unit(b);
    char t[600];
    int n = 0;
    t[n++] = '\n';
    n += snprintf(t + n, sizeof t - (size_t)n, "%.*s", min2(ind, 256), l->s);
    if (deeper) n += snprintf(t + n, sizeof t - (size_t)n, "%s", u);
    int mid_y = -1, mid_x = -1;
    if (prev == '{' && next == '}') {         /* magic newline inside {} */
        mid_y = b->cy + 1;
        mid_x = n - 1;
        n += snprintf(t + n, sizeof t - (size_t)n, "\n%.*s", min2(ind, 256), l->s);
    }
    edit_ins(b, b->cy, b->cx, t, n);
    if (mid_y >= 0) { b->cy = mid_y; b->cx = ind + (int)strlen(u); (void)mid_x; }
}
void ed_backspace(Buf *b) {
    if (b->sel) { begin_action(AK_OTHER); sel_delete(b); return; }
    begin_action(AK_BS);
    Line *l = &b->ln[b->cy];
    if (b->cx > 0) {
        /* delete both halves of an empty auto-closed pair */
        if (b->cx < l->len) {
            char a = l->s[b->cx - 1], z = l->s[b->cx];
            if ((a == '(' && z == ')') || (a == '[' && z == ']') ||
                (a == '{' && z == '}') ||
                ((a == '"' || a == '\'' || a == '`') && z == a)) {
                edit_del(b, b->cy, b->cx - 1, b->cy, b->cx + 1);
                return;
            }
        }
        edit_del(b, b->cy, utf8_prev(l, b->cx), b->cy, b->cx);
    } else if (b->cy > 0) {
        edit_del(b, b->cy - 1, b->ln[b->cy - 1].len, b->cy, 0);
    }
}
void ed_delete(Buf *b) {
    if (b->sel) { begin_action(AK_OTHER); sel_delete(b); return; }
    begin_action(AK_OTHER);
    Line *l = &b->ln[b->cy];
    if (b->cx < l->len) edit_del(b, b->cy, b->cx, b->cy, utf8_next(l, b->cx));
    else if (b->cy < b->n - 1) edit_del(b, b->cy, b->cx, b->cy + 1, 0);
}
void ed_tab(Buf *b, int dedent) {
    begin_action(AK_OTHER);
    int y1, x1, y2, x2;
    const char *u = indent_unit(b);
    int ul = (int)strlen(u);
    /* sel_norm() leaves the coords untouched when it returns 0 (which
     * includes an active-but-empty selection), so seed them first and
     * branch on its result rather than on b->sel. */
    int had_sel = sel_norm(b, &y1, &x1, &y2, &x2);
    if (!had_sel) { y1 = y2 = b->cy; x1 = x2 = b->cx; b->sel = 0; }
    if (had_sel || dedent) {
        if (had_sel && x2 == 0 && y2 > y1) y2--;   /* don't touch empty tail */
        for (int y = y1; y <= y2; y++) {
            Line *l = &b->ln[y];
            if (dedent) {
                int cut = 0;
                if (l->len && l->s[0] == '\t') cut = 1;
                else while (cut < ul && cut < l->len && l->s[cut] == ' ') cut++;
                if (cut) edit_del(b, y, 0, y, cut);
            } else if (l->len) {
                edit_ins(b, y, 0, u, ul);
            }
        }
        if (had_sel) { b->ay = y1; b->ax = 0; b->cy = y2; b->cx = b->ln[y2].len; }
        else { b->cy = y1; b->cx = min2(b->cx, b->ln[y1].len); }
        return;
    }
    if (b->lang->soft_tabs) {
        int col = rx_of(&b->ln[b->cy], b->cx);
        int k = tabstop - col % tabstop;
        edit_ins(b, b->cy, b->cx, "        ", k);
    } else edit_ins(b, b->cy, b->cx, "\t", 1);
}
void ed_dup_line(Buf *b) {
    begin_action(AK_OTHER);
    int y1 = b->cy, y2 = b->cy, x1, x2;
    sel_norm(b, &y1, &x1, &y2, &x2);   /* selection => duplicate whole block */
    int tlen;
    char *t = range_text(b, y1, 0, y2, b->ln[y2].len, &tlen);
    char *t2 = xmalloc((size_t)tlen + 2);
    t2[0] = '\n';
    memcpy(t2 + 1, t, (size_t)tlen + 1);
    free(t);
    int savecx = b->cx;
    edit_ins(b, y2, b->ln[y2].len, t2, tlen + 1);
    free(t2);
    b->cy = min2(y2 + (y2 - y1) + 1, b->n - 1);
    b->cx = min2(savecx, b->ln[b->cy].len);
    b->sel = 0;
}
static void ed_del_line(Buf *b) {
    begin_action(AK_OTHER);
    int y1 = b->cy, y2 = b->cy, x1, x2;
    sel_norm(b, &y1, &x1, &y2, &x2);
    b->sel = 0;
    if (y2 < b->n - 1) edit_del(b, y1, 0, y2 + 1, 0);
    else if (y1 > 0)   edit_del(b, y1 - 1, b->ln[y1 - 1].len, y2, b->ln[y2].len);
    else               edit_del(b, 0, 0, y2, b->ln[y2].len);
    b->cx = min2(b->cx, b->ln[b->cy].len);
}
void ed_move_lines(Buf *b, int down) {
    int y1 = b->cy, y2 = b->cy, x1, x2;
    int had_sel = sel_norm(b, &y1, &x1, &y2, &x2);
    if (had_sel && x2 == 0 && y2 > y1) y2--;
    if ((!down && y1 == 0) || (down && y2 >= b->n - 1)) return;
    begin_action(AK_OTHER);
    int savecx = b->cx;
    int tlen;
    char *t = range_text(b, y1, 0, y2, b->ln[y2].len, &tlen);
    /* remove block (with one newline) */
    if (y2 < b->n - 1) edit_del(b, y1, 0, y2 + 1, 0);
    else               edit_del(b, y1 - 1, b->ln[y1 - 1].len, y2, b->ln[y2].len);
    int ny = down ? y1 + 1 : y1 - 1;
    if (ny >= b->n) {                          /* append at very end */
        char *t2 = xmalloc((size_t)tlen + 2);
        t2[0] = '\n'; memcpy(t2 + 1, t, (size_t)tlen + 1);
        edit_ins(b, b->n - 1, b->ln[b->n - 1].len, t2, tlen + 1);
        free(t2);
        ny = b->n - (y2 - y1 + 1);
    } else {
        char *t2 = xmalloc((size_t)tlen + 2);
        memcpy(t2, t, (size_t)tlen);
        t2[tlen] = '\n'; t2[tlen + 1] = 0;
        edit_ins(b, ny, 0, t2, tlen + 1);
        free(t2);
    }
    free(t);
    int nlines = y2 - y1;
    if (had_sel) {
        b->sel = 1; b->ay = ny; b->ax = 0;
        b->cy = ny + nlines; b->cx = b->ln[b->cy].len;
    } else {
        b->cy = ny; b->cx = min2(savecx, b->ln[ny].len);
    }
}
void ed_toggle_comment(Buf *b) {
    const char *tok = b->lang->lc[0] ? b->lang->lc : NULL;
    if (!tok) { set_msg("no line comment for %s", b->lang->name); return; }
    int tl = (int)strlen(tok);
    begin_action(AK_OTHER);
    int y1 = b->cy, y2 = b->cy, x1, x2;
    int had_sel = sel_norm(b, &y1, &x1, &y2, &x2);
    if (had_sel && x2 == 0 && y2 > y1) y2--;
    /* all non-empty lines commented? */
    int all = 1, any = 0;
    for (int y = y1; y <= y2; y++) {
        Line *l = &b->ln[y];
        int i = line_indent_len(l);
        if (i >= l->len) continue;
        any = 1;
        if (l->len - i < tl || memcmp(l->s + i, tok, (size_t)tl) != 0) all = 0;
    }
    if (!any) return;
    for (int y = y1; y <= y2; y++) {
        Line *l = &b->ln[y];
        int i = line_indent_len(l);
        if (i >= l->len) continue;
        if (all) {
            int cut = tl;
            if (i + cut < l->len && l->s[i + cut] == ' ') cut++;
            edit_del(b, y, i, y, i + cut);
        } else {
            char t[16];
            snprintf(t, sizeof t, "%s ", tok);
            edit_ins(b, y, i, t, tl + 1);
        }
    }
    if (had_sel) { b->sel = 1; b->ay = y1; b->ax = 0; b->cy = y2; b->cx = b->ln[y2].len; }
    else { b->cy = y1; b->cx = min2(b->cx, b->ln[y1].len); }
}
void ed_copy(Buf *b, int cut) {
    int y1, x1, y2, x2;
    if (sel_norm(b, &y1, &x1, &y2, &x2)) {
        int tlen;
        char *t = range_text(b, y1, x1, y2, x2, &tlen);
        clip_set(t, tlen);
        if (cut) { begin_action(AK_OTHER); sel_delete(b); }
        set_msg(cut ? "cut selection" : "copied selection", NULL);
    } else {                                    /* whole line, VS Code style */
        int tlen;
        char *t = range_text(b, b->cy, 0, b->cy, b->ln[b->cy].len, &tlen);
        char *t2 = xmalloc((size_t)tlen + 2);
        memcpy(t2, t, (size_t)tlen);
        t2[tlen] = '\n'; t2[tlen + 1] = 0;
        free(t);
        clip_set(t2, tlen + 1);
        if (cut) ed_del_line(b);
        set_msg(cut ? "cut line" : "copied line", NULL);
    }
}
void ed_paste_text(Buf *b, const char *t, int len) {
    begin_action(AK_OTHER);
    if (b->sel) sel_delete(b);
    edit_ins(b, b->cy, b->cx, t, len);
}
void ed_open_below(Buf *b) {
    b->sel = 0;
    b->cx = b->ln[b->cy].len;
    ed_enter(b);
}

/* ── bracketed paste ──────────────────────────────────────────────── */
void handle_bracketed_paste(void) {
    size_t cap = 256, n = 0;
    char *t = xmalloc(cap);
    for (;;) {
        int c = getch();
        if (c == K_PEND || c == ERR) break;
        if (c == '\r') c = '\n';
        if (c > 255) continue;
        if (n + 1 >= cap) { cap *= 2; t = xrealloc(t, cap); }
        t[n++] = (char)c;
    }
    t[n] = 0;
    if (cur >= 0 && n) ed_paste_text(tabs[cur], t, (int)n);
    free(t);
}
