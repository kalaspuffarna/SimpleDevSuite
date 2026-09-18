#include "core/sds.h"
#include "ui/ui.h"

/* ── utils ────────────────────────────────────────────────────────── */
void die(const char *m) {
    tc_restore();
    endwin();
    fprintf(stderr, "%s\n", m);
    exit(1);
}
void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n);
    if (!q) die("out of memory");
    return q;
}
void *xmalloc(size_t n) { return xrealloc(NULL, n); }
char *xstrdup(const char *s) {
    char *d = strdup(s);
    if (!d) die("out of memory");
    return d;
}
void set_msg(const char *fmt, const char *a) {
    snprintf(msg, sizeof msg, fmt, a ? a : "");
}
int word_ch(int c) { return isalnum((unsigned char)c) || c == '_' || (unsigned char)c >= 0x80; }
/* Look `name` up on $PATH like a shell would; the full path goes to `out`. */
int find_exec(const char *name, char *out, size_t cap) {
    const char *p = getenv("PATH");
    if (!p || !*p) p = "/usr/bin:/bin";
    while (*p) {
        const char *c = strchr(p, ':');
        size_t n = c ? (size_t)(c - p) : strlen(p);
        if (n && n < cap) {
            char buf[PATH_MAX];
            snprintf(buf, sizeof buf, "%.*s/%s", (int)n, p, name);
            if (access(buf, X_OK) == 0) { snprintf(out, cap, "%s", buf); return 1; }
        }
        if (!c) break;
        p = c + 1;
    }
    return 0;
}
/* Is this directory entry a directory? readdir usually knows, which saves a
 * stat syscall per file — the difference between walking a large tree in
 * milliseconds and in tens of milliseconds. Filesystems that do not report
 * it (some network mounts) say DT_UNKNOWN and we ask the kernel. */
int dirent_is_dir(const struct dirent *e, const char *path) {
#ifdef DT_DIR
    if (e->d_type == DT_DIR) return 1;
    if (e->d_type != DT_UNKNOWN && e->d_type != DT_LNK) return 0;
#endif
    struct stat st;
    if (stat(path, &st) != 0) return -1;         /* unreadable: skip it */
    return S_ISDIR(st.st_mode) ? 1 : 0;
}
int min2(int a, int b) { return a < b ? a : b; }
int max2(int a, int b) { return a > b ? a : b; }
