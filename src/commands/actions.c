#include "core/sds.h"
#include "commands/commands.h"
#include "commands/commands_internal.h"
#include "editor/editor.h"
#include "markdown/markdown.h"
#include "ui/ui.h"
#include "git/git.h"
#include "terminal/terminal.h"

/* ── app actions ──────────────────────────────────────────────────── */
/* Alt+M. The two views keep their own place in the file, so switching
 * carries the reading position across rather than jumping to the top. */
void md_toggle(void) {
    if (cur < 0) { set_msg("open a markdown file first%s", ""); return; }
    Buf *b = tabs[cur];
    if (!md_is_md(b)) {
        set_msg("not a markdown file — %s", b->name);
        return;
    }
    if (b->md_view) {
        if (b->md && b->md->n) {
            int sy = b->md->src[min2(b->md->rowoff, b->md->n - 1)];
            if (sy >= 0 && sy < b->n) { b->cy = sy; b->cx = 0; }
        }
        b->md_view = 0;
        set_msg("markdown source — Alt+M renders it%s", "");
    } else {
        b->md_view = 1;
        b->md_goto = b->cy;
        b->sel = 0;
        set_msg("rendered markdown — Alt+M shows the source%s", "");
    }
}
void md_scroll(Buf *b, int dy) {
    Md *m = b->md;
    if (!m || m->n == 0) return;
    long to = (long)m->rowoff + dy;             /* Home/End pass INT_MAX */
    m->rowoff = (int)(to < 0 ? 0 : to > m->n - 1 ? m->n - 1 : to);
}
void act_save(void) {
    if (cur < 0) return;
    if (tabs[cur]->kind != TAB_FILE) { set_msg("nothing to save here", NULL); return; }
    if (buf_save(tabs[cur]) == 0) {
        set_msg("saved %s", tabs[cur]->name);
        git_refresh();                     /* the file's status just changed */
    } else set_msg("save failed: %s", tabs[cur]->path);
}
void act_close(void) {
    if (cur < 0) return;
    if (tabs[cur]->kind == TAB_TERM) {
        /* a still-running shell is worth one confirmation */
        Term *t = tabs[cur]->term;
        if (t && !t->dead && !pending_close) {
            pending_close = 1;
            set_msg("shell still running — Alt+W again to kill it", NULL);
            return;
        }
        close_tab(cur);
        pending_close = 0;
        return;
    }
    if (tabs[cur]->dirty && !pending_close) {
        pending_close = 1;
        set_msg("unsaved changes — Alt+W again to discard", NULL);
        return;
    }
    close_tab(cur);
    pending_close = 0;
}
int act_quit(void) {
    int dirty = 0, shells = 0;
    for (int i = 0; i < ntabs; i++) {
        if (tabs[i]->kind == TAB_TERM) {
            if (tabs[i]->term && !tabs[i]->term->dead) shells++;
        } else dirty |= tabs[i]->dirty;
    }
    if ((dirty || shells) && !pending_quit) {
        pending_quit = 1;
        if (dirty) set_msg("unsaved changes — Alt+Q again to quit anyway", NULL);
        else       set_msg("shells still running — Alt+Q again to quit", NULL);
        return 0;
    }
    return 1;
}
