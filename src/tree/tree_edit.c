#include "core/sds.h"
#include "tree/tree.h"
#include "tree/tree_internal.h"
#include "ui/ui.h"

/* ── new file / folder in the tree ────────────────────────────────── */
/* insert `k` into `d`'s child list, keeping the dirs-first sort */
static void node_add_child(Node *d, Node *k) {
    if (d->nkid == d->kidcap) {
        d->kidcap = d->kidcap ? d->kidcap * 2 : 16;
        d->kid = xrealloc(d->kid, (size_t)d->kidcap * sizeof(Node *));
    }
    d->kid[d->nkid++] = k;
    qsort(d->kid, (size_t)d->nkid, sizeof(Node *), node_cmp);
}
void tree_new_entry(void) {
    /* target dir = the selected folder, else the selected file's folder */
    Node *d = root;
    if (nvis) d = vis[tsel]->is_dir ? vis[tsel] : vis[tsel]->parent;
    if (!d) d = root;

    const char *shown = d == root ? "." : d->name;
    char title[NAME_MAX + 48];
    snprintf(title, sizeof title, "New entry in %s/", shown);

    char name[NAME_MAX + 1] = "";
    if (!input_box(title, "end with / to make a folder — Esc cancels",
                   name, sizeof name)) {
        set_msg("cancelled", NULL);
        return;
    }
    int is_dir = 0;
    size_t nl = strlen(name);
    while (nl && name[nl - 1] == '/') { is_dir = 1; name[--nl] = 0; }
    if (!nl) { set_msg("empty name", NULL); return; }
    if (strchr(name, '/') || !strcmp(name, ".") || !strcmp(name, "..")) {
        set_msg("invalid name: %s", name);
        return;
    }

    char path[PATH_MAX];
    if (snprintf(path, sizeof path, "%s/%s", d->path, name) >= (int)sizeof path) {
        set_msg("path too long", NULL);
        return;
    }
    /* load the folder BEFORE touching disk, or node_load() would pick the
     * new entry up too and we'd graft a duplicate below */
    if (!d->loaded) node_load(d);

    struct stat st;
    if (stat(path, &st) == 0) {
        if (!is_dir && S_ISREG(st.st_mode)) {      /* already there: just open */
            open_file(path);
            set_msg("already exists, opened %s", name);
            return;
        }
        set_msg("already exists: %s", name);
        return;
    }
    if (is_dir) {
        if (mkdir(path, 0755) != 0) { set_msg("could not create %s", name); return; }
    } else {
        FILE *f = fopen(path, "w");
        if (!f) { set_msg("could not create %s", name); return; }
        fclose(f);
    }

    /* graft into the tree without losing other folders' expanded state */
    Node *k = NULL;
    for (int i = 0; i < d->nkid; i++)
        if (!strcmp(d->kid[i]->name, name)) { k = d->kid[i]; break; }
    if (!k) {
        k = node_new(name, path, is_dir, d);
        if (is_dir) k->loaded = 1;
        node_add_child(d, k);
    }
    if (d != root) d->expanded = 1;
    tree_rebuild();
    for (int i = 0; i < nvis; i++) if (vis[i] == k) { tsel = i; break; }

    if (!is_dir) open_file(path);
    set_msg(is_dir ? "created folder %s" : "created %s", name);
}

/* ── delete from the tree ─────────────────────────────────────────── */
/* how many entries live under `dir` (capped — we only need "1 or many") */
static void count_tree(const char *dir, int *files, int *dirs, int depth) {
    if (depth > 16 || *files + *dirs > 5000) return;
    DIR *dp = opendir(dir);
    if (!dp) return;
    struct dirent *e;
    while ((e = readdir(dp))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char p[PATH_MAX];
        if (snprintf(p, sizeof p, "%s/%s", dir, e->d_name) >= (int)sizeof p) continue;
        struct stat st;
        if (lstat(p, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) { (*dirs)++; count_tree(p, files, dirs, depth + 1); }
        else (*files)++;
    }
    closedir(dp);
}
static int rm_rf(const char *path, int depth) {
    struct stat st;
    if (lstat(path, &st) != 0) return -1;
    if (!S_ISDIR(st.st_mode)) return unlink(path);
    if (depth > 16) return -1;
    DIR *dp = opendir(path);
    if (!dp) return -1;
    struct dirent *e;
    int rc = 0;
    while ((e = readdir(dp))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char p[PATH_MAX];
        if (snprintf(p, sizeof p, "%s/%s", path, e->d_name) >= (int)sizeof p) {
            rc = -1; continue;
        }
        if (rm_rf(p, depth + 1) != 0) rc = -1;
    }
    closedir(dp);
    return rmdir(path) != 0 ? -1 : rc;
}
static void node_unlink(Node *n) {               /* detach from parent */
    Node *p = n->parent;
    if (!p) return;
    for (int i = 0; i < p->nkid; i++)
        if (p->kid[i] == n) {
            memmove(p->kid + i, p->kid + i + 1,
                    (size_t)(p->nkid - i - 1) * sizeof(Node *));
            p->nkid--;
            break;
        }
    node_free(n);
}
/* close any tab whose file lived at (or under) `path` */
static void close_tabs_under(const char *path, int is_dir) {
    size_t pl = strlen(path);
    for (int i = ntabs - 1; i >= 0; i--) {
        if (tabs[i]->kind == TAB_TERM) continue;     /* terminals have no file */
        const char *tp = tabs[i]->path;
        int hit = is_dir ? (strncmp(tp, path, pl) == 0 && tp[pl] == '/')
                         : (strcmp(tp, path) == 0);
        if (hit) close_tab(i);
    }
}
void tree_delete_selected(void) {
    if (nvis == 0) { set_msg("nothing selected", NULL); return; }
    Node *n = vis[tsel];

    char title[NAME_MAX + 64], detail[256];
    if (n->is_dir) {
        int files = 0, dirs = 0;
        count_tree(n->path, &files, &dirs, 0);
        snprintf(title, sizeof title, "Really delete folder \"%s\"?", n->name);
        if (files || dirs)
            snprintf(detail, sizeof detail,
                     "%d file(s) and %d folder(s) inside will be lost.", files, dirs);
        else
            snprintf(detail, sizeof detail, "The folder is empty.");
    } else {
        int open_dirty = 0;
        for (int i = 0; i < ntabs; i++)
            if (!strcmp(tabs[i]->path, n->path) && tabs[i]->dirty) open_dirty = 1;
        snprintf(title, sizeof title, "Really delete \"%s\"?", n->name);
        snprintf(detail, sizeof detail, "%s",
                 open_dirty ? "It is open with unsaved changes."
                            : "This cannot be undone.");
    }
    if (!confirm(title, detail, 1)) { set_msg("delete cancelled", NULL); return; }

    char path[PATH_MAX], name[NAME_MAX + 1];
    snprintf(path, sizeof path, "%s", n->path);
    snprintf(name, sizeof name, "%s", n->name);
    int is_dir = n->is_dir;

    if (rm_rf(path, 0) != 0) { set_msg("could not delete %s", name); return; }

    close_tabs_under(path, is_dir);
    node_unlink(n);
    tree_rebuild();
    if (tsel >= nvis) tsel = max2(0, nvis - 1);
    set_msg("deleted %s", name);
}
