#include "core/sds.h"
#include "tree/tree.h"
#include "tree/tree_internal.h"
#include "ui/ui.h"
#include "git/git.h"

int tree_w  = 30;           /* file-tree sidebar width in columns */
int tree_autohide = 80;     /* hide the sidebar below this COLS (0=never) */
int tree_hidden = 0;        /* sidebar currently folded away */

Node *root = NULL;
Node **vis = NULL;
int   nvis = 0, viscap = 0, tsel = 0, toff = 0;

/* ── file tree ────────────────────────────────────────────────────── */
Node *node_new(const char *name, const char *path, int is_dir, Node *parent) {
    Node *n = calloc(1, sizeof *n);
    if (!n) die("out of memory");
    n->name = xstrdup(name); n->path = xstrdup(path);
    n->is_dir = is_dir; n->parent = parent;
    n->depth = parent ? parent->depth + 1 : -1;
    return n;
}
int node_cmp(const void *a, const void *b) {
    const Node *x = *(Node * const *)a, *y = *(Node * const *)b;
    if (x->is_dir != y->is_dir) return y->is_dir - x->is_dir;
    return strcasecmp(x->name, y->name);
}
void node_load(Node *d) {
    if (d->loaded) return;
    d->loaded = 1;
    DIR *dp = opendir(d->path);
    if (!dp) return;
    struct dirent *e;
    while ((e = readdir(dp))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char p[PATH_MAX];
        if (snprintf(p, sizeof p, "%s/%s", d->path, e->d_name) >= (int)sizeof p)
            continue;
        int isdir = dirent_is_dir(e, p);
        if (isdir < 0) continue;
        Node *k = node_new(e->d_name, p, isdir, d);
        if (d->nkid == d->kidcap) {          /* doubling, not one per entry */
            d->kidcap = d->kidcap ? d->kidcap * 2 : 16;
            d->kid = xrealloc(d->kid, (size_t)d->kidcap * sizeof(Node *));
        }
        d->kid[d->nkid++] = k;
    }
    closedir(dp);
    qsort(d->kid, (size_t)d->nkid, sizeof(Node *), node_cmp);
}
void node_free(Node *n) {
    for (int i = 0; i < n->nkid; i++) node_free(n->kid[i]);
    free(n->kid); free(n->name); free(n->path);
    free(n);
}
/* Re-read `d` from disk, reusing nodes that are still there so expanded
 * folders stay expanded. Unloaded folders are left alone — they'll read
 * fresh whenever they're first expanded. */
static void node_refresh(Node *d) {
    if (!d->loaded) return;
    Node **old = d->kid;
    int nold = d->nkid;
    d->kid = NULL;                   /* the capacity goes with the array */
    d->nkid = d->kidcap = 0;

    DIR *dp = opendir(d->path);
    if (!dp) {                       /* folder vanished under us */
        for (int i = 0; i < nold; i++) node_free(old[i]);
        free(old);
        d->loaded = 0;
        return;
    }
    /* `old` is still in node_cmp order, so a surviving node is a binary
     * search away. Scanning the whole child list for every entry read made
     * refreshing a directory cost its size squared — noticeable from a few
     * thousand files up. */
    char *used = nold ? calloc((size_t)nold, 1) : NULL;
    if (nold && !used) die("out of memory");
    struct dirent *e;
    while ((e = readdir(dp))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char p[PATH_MAX];
        if (snprintf(p, sizeof p, "%s/%s", d->path, e->d_name) >= (int)sizeof p)
            continue;
        int is_dir = dirent_is_dir(e, p);
        if (is_dir < 0) continue;

        Node probe, *pp = &probe, *k = NULL;
        probe.name = (char *)e->d_name;
        probe.is_dir = is_dir;
        int lo = 0, hi = nold - 1;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            int c = node_cmp(&old[mid], &pp);
            if (c == 0) {
                /* the order ignores case, so names that differ only in case
                 * land together; walk the run for the exact one */
                for (int j = mid; j >= 0 && node_cmp(&old[j], &pp) == 0; j--)
                    if (!used[j] && !strcmp(old[j]->name, e->d_name)) {
                        k = old[j]; used[j] = 1; break;
                    }
                for (int j = mid + 1; !k && j < nold && node_cmp(&old[j], &pp) == 0; j++)
                    if (!used[j] && !strcmp(old[j]->name, e->d_name)) {
                        k = old[j]; used[j] = 1; break;
                    }
                break;
            }
            if (c < 0) lo = mid + 1; else hi = mid - 1;
        }
        if (!k) k = node_new(e->d_name, p, is_dir, d);
        else if (k->is_dir) node_refresh(k);    /* recurse into loaded dirs */

        if (d->nkid == d->kidcap) {
            d->kidcap = d->kidcap ? d->kidcap * 2 : 16;
            d->kid = xrealloc(d->kid, (size_t)d->kidcap * sizeof(Node *));
        }
        d->kid[d->nkid++] = k;
    }
    closedir(dp);
    for (int i = 0; i < nold; i++) if (!used[i]) node_free(old[i]);  /* gone */
    free(used);
    free(old);
    qsort(d->kid, (size_t)d->nkid, sizeof(Node *), node_cmp);
}

static void flatten(Node *d) {
    for (int i = 0; i < d->nkid; i++) {
        Node *k = d->kid[i];
        if (nvis == viscap) {
            viscap = viscap ? viscap * 2 : 128;
            vis = xrealloc(vis, (size_t)viscap * sizeof(Node *));
        }
        vis[nvis++] = k;
        if (k->is_dir && k->expanded) flatten(k);
    }
}
void tree_rebuild(void) {
    nvis = 0;
    flatten(root);
    if (tsel >= nvis) tsel = nvis - 1;
    if (tsel < 0) tsel = 0;
}
void tree_open_selected(void) {
    if (nvis == 0) return;
    Node *n = vis[tsel];
    if (n->is_dir) {
        n->expanded = !n->expanded;
        if (n->expanded) node_load(n);
        tree_rebuild();
    } else open_file(n->path);
}
/* Alt+Shift+Enter: open the selected file beside whatever is already on
 * screen instead of on top of it. Directories have no pane meaning, so they
 * just expand as usual. */
void tree_open_pane_selected(void) {
    if (nvis == 0) return;
    Node *n = vis[tsel];
    if (n->is_dir) { tree_open_selected(); return; }
    for (int i = 0; i < ntabs; i++)         /* already in a pane → go there */
        if (tabs[i]->kind != TAB_TERM && strcmp(tabs[i]->path, n->path) == 0)
            for (int p = 0; p < npanes; p++)
                if (panes[p] == i) { curpane = p; cur = i; return; }
    if (cur < 0) { open_file(n->path); return; }   /* nothing open, no split */
    if (npanes >= MAX_PANES) { set_msg("all panes are in use", NULL); return; }
    /* Split first, then open into the new pane: open_file() sets the focused
     * pane, and the focused pane must not be the one we are splitting from. */
    int from = panes[curpane];
    panes[npanes] = from;
    curpane = npanes++;
    cur = from;
    open_file(n->path);
    if (panes[curpane] == from) pane_close();      /* it failed — undo the split */
}
/* The tree keys work on the sidebar, so they need it on screen: moving a
 * selection nobody can see — and then opening or deleting whatever it landed
 * on — is a good way to lose a file by accident. Alt+B and Alt+Right stay
 * live while it is hidden, since those are what bring it back. */
int tree_active(void) {
    if (!tree_hidden) return 1;
    set_msg("sidebar is hidden — Alt+B or Alt+Right brings it back", NULL);
    return 0;
}
void tree_toggle(void) {
    tree_hidden = !tree_hidden;
    set_msg(tree_hidden ? "sidebar hidden" : "sidebar shown", NULL);
}
/* Collapse one level. Collapsing when there is nothing left to collapse —
 * a top-level entry, already folded — folds the sidebar itself away, so
 * repeated Alt+Left in the root directory reclaims the whole width. */
void tree_collapse(void) {
    if (!tree_active()) return;
    if (nvis == 0) { tree_hidden = 1; return; }
    Node *n = vis[tsel];
    if (n->is_dir && n->expanded) { n->expanded = 0; tree_rebuild(); return; }
    if (n->parent && n->parent != root) {
        for (int i = 0; i < nvis; i++)
            if (vis[i] == n->parent) { tsel = i; break; }
        return;
    }
    tree_hidden = 1;
    set_msg("sidebar hidden — Alt+Right or Alt+B to bring it back", NULL);
}

/* rescan the whole tree, keeping the cursor on the same path if it survived */
void tree_refresh(void) {
    git_refresh();
    char keep[PATH_MAX] = "";
    if (nvis) snprintf(keep, sizeof keep, "%s", vis[tsel]->path);
    node_refresh(root);
    tree_rebuild();
    if (keep[0])
        for (int i = 0; i < nvis; i++)
            if (!strcmp(vis[i]->path, keep)) { tsel = i; break; }
    if (tsel >= nvis) tsel = max2(0, nvis - 1);
}
void tree_expand(void) {
    if (tree_hidden) { tree_hidden = 0; return; }   /* first Alt+Right un-hides */
    if (nvis == 0) return;
    Node *n = vis[tsel];
    if (n->is_dir && !n->expanded) { n->expanded = 1; node_load(n); tree_rebuild(); }
}
