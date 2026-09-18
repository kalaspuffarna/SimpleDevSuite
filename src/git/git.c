#include "core/sds.h"
#include "git/git.h"
#include "tree/tree.h"

/* ── git status ───────────────────────────────────────────────────── */
/* One `git status --porcelain` per refresh, cached in a flat sorted table
 * that the tree draw looks up by path. Directories inherit the "contains
 * something interesting" marker from their children. */
typedef struct { char *path; char st; } GitEnt;

static GitEnt *gitent = NULL;
static int     ngit = 0, gitcap = 0;
int     git_repo = 0;
char    git_branch[128] = "";

static int gitent_cmp(const void *a, const void *b) {
    return strcmp(((const GitEnt *)a)->path, ((const GitEnt *)b)->path);
}
/* Wrap a path for the shell. A directory name may legitimately contain a
 * quote, so close/escape/reopen rather than trusting the input. Returns 0 if
 * the result would not fit, in which case the caller skips the command. */
static int shq(char *out, size_t cap, const char *s) {
    size_t n = 0;
    if (cap < 3) return 0;
    out[n++] = '\'';
    for (; *s; s++) {
        if (*s == '\'') {
            if (n + 4 >= cap) return 0;
            memcpy(out + n, "'\\''", 4);
            n += 4;
        } else {
            if (n + 1 >= cap) return 0;
            out[n++] = *s;
        }
    }
    if (n + 2 > cap) return 0;
    out[n++] = '\'';
    out[n] = 0;
    return 1;
}
static void git_clear(void) {
    for (int i = 0; i < ngit; i++) free(gitent[i].path);
    ngit = 0;
    git_repo = 0;
    git_branch[0] = 0;
}
static void git_add(const char *rel, char st) {
    if (ngit == gitcap) {
        gitcap = gitcap ? gitcap * 2 : 128;
        gitent = xrealloc(gitent, (size_t)gitcap * sizeof *gitent);
    }
    gitent[ngit].path = xstrdup(rel);
    gitent[ngit].st = st;
    ngit++;
}
/* Reduce a two-column porcelain code to the single letter shown in the tree. */
static char git_code(const char *xy) {
    char x = xy[0], y = xy[1];
    if (x == '?' || y == '?') return '?';
    if (x == 'A' || y == 'A') return 'A';
    if (x == 'D' || y == 'D') return 'D';
    if (x == 'R' || y == 'R') return 'R';
    if (x != ' ' && x != '?') return 'S';      /* staged */
    return 'M';
}
void git_refresh(void) {
    git_clear();
    if (!root) return;

    char qroot[PATH_MAX * 2 + 8], cmd[PATH_MAX * 2 + 96];
    if (!shq(qroot, sizeof qroot, root->path)) return;
    snprintf(cmd, sizeof cmd,
             "git -C %s rev-parse --abbrev-ref HEAD 2>/dev/null", qroot);
    FILE *f = popen(cmd, "r");
    if (f) {
        if (fgets(git_branch, sizeof git_branch, f)) {
            git_branch[strcspn(git_branch, "\r\n")] = 0;
            if (git_branch[0]) git_repo = 1;
        }
        pclose(f);
    }
    if (!git_repo) return;

    snprintf(cmd, sizeof cmd,
             "git -C %s status --porcelain 2>/dev/null", qroot);
    f = popen(cmd, "r");
    if (!f) return;
    char line[PATH_MAX + 8];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (strlen(line) < 4) continue;
        char st = git_code(line);
        char *p = line + 3;
        char *arrow = strstr(p, " -> ");      /* renames: take the new name */
        if (arrow) p = arrow + 4;
        size_t n = strlen(p);
        if (n && p[n - 1] == '/') p[n - 1] = 0;   /* untracked dir */
        if (*p == '"') {                          /* quoted path */
            p++;
            char *e = strrchr(p, '"');
            if (e) *e = 0;
        }
        git_add(p, st);
    }
    pclose(f);
    qsort(gitent, (size_t)ngit, sizeof *gitent, gitent_cmp);
}
/* Status letter for an absolute path, or 0. Directories report the status of
 * whatever lies beneath them so a change is visible while collapsed. */
char git_status_for(const char *abs, int is_dir) {
    if (!git_repo || !ngit || !root) return 0;
    size_t rl = strlen(root->path);
    if (strncmp(abs, root->path, rl) != 0) return 0;
    const char *rel = abs + rl;
    while (*rel == '/') rel++;
    if (!*rel) return 0;

    if (!is_dir) {
        GitEnt key;
        key.path = (char *)rel;
        GitEnt *hit = bsearch(&key, gitent, (size_t)ngit, sizeof *gitent, gitent_cmp);
        return hit ? hit->st : 0;
    }
    /* Directory: anything under "rel/" counts. The table is sorted, so the
     * first entry at or after that prefix answers it — the tree asks once per
     * visible row per frame, and scanning every changed file each time cost
     * milliseconds a frame in a repo with a few thousand of them. */
    char pre[PATH_MAX];
    int n = snprintf(pre, sizeof pre, "%s/", rel);
    if (n < 0 || n >= (int)sizeof pre) return 0;
    int lo = 0, hi = ngit;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (strcmp(gitent[mid].path, pre) < 0) lo = mid + 1;
        else hi = mid;
    }
    if (lo < ngit && strncmp(gitent[lo].path, pre, (size_t)n) == 0)
        return gitent[lo].st == '?' ? '?' : 'M';
    return 0;
}
