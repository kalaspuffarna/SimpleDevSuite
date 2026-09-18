#include "core/sds.h"
#include "syntax/syntax.h"
#include "syntax/syntax_internal.h"

/* ── tree-sitter (optional) ───────────────────────────────────────── */
/* Built only with -DSDS_TREESITTER. Grammars are dlopen'd at runtime rather
 * than linked, so sds keeps working when a language's grammar is missing —
 * it just falls back to the keyword lexer in lexer.c. Nothing here is required
 * for sds to build or run.
 *
 *   grammars: $XDG_DATA_HOME/sds/grammars/libtree-sitter-<lang>.so, then
 *             the usual system library directories
 *   queries:  $XDG_DATA_HOME/sds/queries/<lang>/highlights.scm
 *
 * `sds --fetch-grammar <lang>` builds and installs both.                  */
#ifdef SDS_TREESITTER
typedef struct {
    char         name[24];
    const TSLanguage *lang;
    TSQuery     *query;
    unsigned char *cap;        /* query capture index → HA_* */
    int          tried;        /* load attempted; don't retry every keystroke */
} TSGram;

static TSGram tsgram[NLANGS];

/* sds language name → tree-sitter grammar name (mostly identity) */
static const char *ts_name_of(const Lang *lg) {
    if (!strcmp(lg->name, "c++"))  return "cpp";
    if (!strcmp(lg->name, "make")) return "make";
    if (!strcmp(lg->name, "text")) return NULL;
    return lg->name;
}
/* tree-sitter capture names are dotted and open-ended ("keyword.coroutine",
 * "type.builtin", …); classify on the leading component. */
static unsigned char ts_cap_attr(const char *nm, uint32_t len) {
    char b[64];
    uint32_t n = len < sizeof b - 1 ? len : (uint32_t)sizeof b - 1;
    memcpy(b, nm, n); b[n] = 0;
    char *dot = strchr(b, '.');
    if (dot) *dot = 0;
    if (!strcmp(b, "keyword"))                          return HA_KW;
    if (!strcmp(b, "type") || !strcmp(b, "constructor")) return HA_TYPE;
    if (!strcmp(b, "function") || !strcmp(b, "method")) return HA_TYPE;
    if (!strcmp(b, "string") || !strcmp(b, "character")) return HA_STR;
    if (!strcmp(b, "comment"))                          return HA_COM;
    if (!strcmp(b, "number") || !strcmp(b, "float"))    return HA_NUM;
    if (!strcmp(b, "preproc") || !strcmp(b, "keyword_directive")) return HA_PRE;
    if (!strcmp(b, "constant")) {
        return (dot && !strcmp(dot + 1, "numeric")) ? HA_NUM : HA_TYPE;
    }
    return HA_DEF;
}
static void ts_data_dir(char *out, size_t cap, const char *sub) {
    const char *xdg = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
    if (xdg && *xdg) snprintf(out, cap, "%s/sds/%s", xdg, sub);
    else if (home && *home) snprintf(out, cap, "%s/.local/share/sds/%s", home, sub);
    else snprintf(out, cap, "./%s", sub);
}
static char *slurp(const char *path, uint32_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    if (n < 0 || n > (1 << 22)) { fclose(f); return NULL; }
    rewind(f);
    char *s = xmalloc((size_t)n + 1);
    size_t got = fread(s, 1, (size_t)n, f);
    fclose(f);
    s[got] = 0;
    *len = (uint32_t)got;
    return s;
}
/* A grammar's highlight query is usually only the part that differs from a
 * base language: tree-sitter-cpp's file has no rules for `int` or `char8_t`
 * because those live in tree-sitter-c's. Upstream expresses that with a
 * "; inherits: c" comment (which not every repo actually includes), so honor
 * the directive and also fall back to a known parent per language. */
static const char *ts_parent_of(const char *nm) {
    if (!strcmp(nm, "cpp"))        return "c";
    if (!strcmp(nm, "typescript")) return "javascript";
    if (!strcmp(nm, "tsx"))        return "typescript";
    return NULL;
}
/* Read <nm>'s query, prefixed by whatever it inherits. Depth-capped so a
 * malformed inherits cycle cannot recurse forever. */
static char *ts_query_src(const char *nm, uint32_t *outlen, int depth) {
    char dir[PATH_MAX - 64], path[PATH_MAX];
    ts_data_dir(dir, sizeof dir, "queries");
    snprintf(path, sizeof path, "%s/%s/highlights.scm", dir, nm);
    uint32_t len = 0;
    char *own = slurp(path, &len);
    if (!own) return NULL;
    if (depth >= 4) { *outlen = len; return own; }

    /* explicit "; inherits: a,b" on one of the first lines */
    char parents[128] = "";
    const char *ih = strstr(own, "inherits:");
    if (ih && ih - own < 200) {
        ih += 9;
        while (*ih == ' ') ih++;
        size_t k = 0;
        while (*ih && *ih != '\n' && k + 1 < sizeof parents) parents[k++] = *ih++;
        parents[k] = 0;
    }
    if (!parents[0]) {
        const char *p = ts_parent_of(nm);
        if (p) snprintf(parents, sizeof parents, "%s", p);
    }
    if (!parents[0]) { *outlen = len; return own; }

    /* concatenate each parent's query ahead of this one */
    char *acc = NULL;
    uint32_t acclen = 0;
    char *save = NULL;
    for (char *tok = strtok_r(parents, ", \t", &save); tok;
         tok = strtok_r(NULL, ", \t", &save)) {
        if (!strcmp(tok, nm)) continue;
        uint32_t plen = 0;
        char *ps = ts_query_src(tok, &plen, depth + 1);
        if (!ps) continue;
        acc = xrealloc(acc, acclen + plen + 2);
        memcpy(acc + acclen, ps, plen);
        acclen += plen;
        acc[acclen++] = '\n';
        free(ps);
    }
    if (!acc) { *outlen = len; return own; }
    acc = xrealloc(acc, acclen + len + 1);
    memcpy(acc + acclen, own, len);
    acclen += len;
    acc[acclen] = 0;
    free(own);
    *outlen = acclen;
    return acc;
}
/* Load grammar + highlight query for `lg`, once. Returns NULL if either is
 * unavailable, which simply means this buffer keeps using the lexer. */
static TSGram *ts_for(const Lang *lg) {
    int li = (int)(lg - langs);
    if (li < 0 || li >= NLANGS) return NULL;
    TSGram *g = &tsgram[li];
    if (g->tried) return g->lang && g->query ? g : NULL;
    g->tried = 1;

    const char *nm = ts_name_of(lg);
    if (!nm) return NULL;
    snprintf(g->name, sizeof g->name, "%s", nm);

    /* the directory is bounded well below PATH_MAX so the composed paths
     * below always fit — sized explicitly to keep -Wformat-truncation quiet */
    char dir[PATH_MAX - 64], sym[64];
    ts_data_dir(dir, sizeof dir, "grammars");
    const char *cands[4];
    char c0[PATH_MAX], c1[64 + 24], c2[64 + 24];
    snprintf(c0, sizeof c0, "%s/libtree-sitter-%s.so", dir, nm);
    snprintf(c1, sizeof c1, "libtree-sitter-%s.so", nm);        /* ld.so path */
    snprintf(c2, sizeof c2, "/usr/lib/libtree-sitter-%s.so", nm);
    cands[0] = c0; cands[1] = c1; cands[2] = c2; cands[3] = NULL;

    void *dl = NULL;
    for (int i = 0; cands[i] && !dl; i++) dl = dlopen(cands[i], RTLD_LAZY | RTLD_LOCAL);
    if (!dl) return NULL;

    snprintf(sym, sizeof sym, "tree_sitter_%s", nm);
    const TSLanguage *(*fn)(void) = (const TSLanguage *(*)(void))
        (uintptr_t)dlsym(dl, sym);
    if (!fn) { dlclose(dl); return NULL; }
    g->lang = fn();
    if (!g->lang) { dlclose(dl); return NULL; }

    uint32_t qlen = 0;
    char *src = ts_query_src(nm, &qlen, 0);
    if (!src) { g->lang = NULL; dlclose(dl); return NULL; }

    uint32_t erroff; TSQueryError errtype;
    g->query = ts_query_new(g->lang, src, qlen, &erroff, &errtype);
    free(src);
    if (!g->query) {
        /* An inherited query can reference node types this grammar does not
         * have, which fails the whole compile. Retry with just this
         * language's own rules before giving up on tree-sitter entirely. */
        char dir[PATH_MAX - 64], path[PATH_MAX];
        ts_data_dir(dir, sizeof dir, "queries");
        snprintf(path, sizeof path, "%s/%s/highlights.scm", dir, nm);
        src = slurp(path, &qlen);
        if (src) {
            g->query = ts_query_new(g->lang, src, qlen, &erroff, &errtype);
            free(src);
        }
    }
    if (!g->query) { g->lang = NULL; dlclose(dl); return NULL; }

    uint32_t nc = ts_query_capture_count(g->query);
    g->cap = xmalloc(nc ? nc : 1);
    for (uint32_t i = 0; i < nc; i++) {
        uint32_t l;
        const char *cn = ts_query_capture_name_for_id(g->query, i, &l);
        g->cap[i] = ts_cap_attr(cn, l);
    }
    return g;
}

/* byte offset of the start of each line, rebuilt when the buffer changes */
void ts_offsets(Buf *b) {
    if (!b->ts_off_dirty && b->ts_off) return;
    b->ts_off = xrealloc(b->ts_off, (size_t)(b->n + 1) * sizeof *b->ts_off);
    uint32_t o = 0;
    for (int i = 0; i < b->n; i++) { b->ts_off[i] = o; o += (uint32_t)b->ln[i].len + 1; }
    b->ts_off[b->n] = o;
    b->ts_bytes = o;
    b->ts_off_dirty = 0;
}
/* feed the line array to tree-sitter without flattening it into one string */
static const char *ts_read(void *payload, uint32_t byte, TSPoint pt, uint32_t *len) {
    (void)pt;
    Buf *b = payload;
    if (byte >= b->ts_bytes) { *len = 0; return ""; }
    int lo = 0, hi = b->n - 1, li = 0;
    while (lo <= hi) {                       /* which line holds `byte`? */
        int mid = (lo + hi) / 2;
        if (b->ts_off[mid] <= byte) { li = mid; lo = mid + 1; } else hi = mid - 1;
    }
    uint32_t within = byte - b->ts_off[li];
    if (within < (uint32_t)b->ln[li].len) {
        *len = (uint32_t)b->ln[li].len - within;
        return b->ln[li].s + within;
    }
    *len = 1;                                 /* the line's newline */
    return "\n";
}
/* Typing re-parses, and on a large file that is far from free, so a burst of
 * keystrokes gets one parse instead of one per key. Between parses the spans
 * from the last one are shifted to follow the edits (ts_shift_spans), so the
 * colors stay where the text went; only the token being typed can be a
 * moment out of date. */
#define TS_DEBOUNCE_MS 120
static long ts_edit_ms;         /* when the buffer was last changed */
static int  ts_due;             /* an edit is waiting for its parse */

static long ts_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
/* -1: nothing owed. Otherwise the milliseconds until the parse is due, which
 * the main loop waits for so the colors settle without a keypress. */
int ts_pending_ms(void) {
    if (!ts_due) return -1;
    long left = TS_DEBOUNCE_MS - (ts_now_ms() - ts_edit_ms);
    return left > 0 ? (int)left : 0;
}
static void ts_reparse(Buf *b) {
    TSGram *g = ts_for(b->lang);
    if (!g) return;
    if (!b->ts_parser) {
        b->ts_parser = ts_parser_new();
        if (!ts_parser_set_language(b->ts_parser, g->lang)) {
            ts_parser_delete(b->ts_parser);
            b->ts_parser = NULL;
            return;
        }
    }
    if (b->ts_tree && b->ts_ver == b->ver) return;      /* still current */
    ts_offsets(b);
    /* A file this size costs more to keep parsed than the better colors are
     * worth; the lexer handles it instead. */
    if (cfg_ts_max_kb > 0 && b->ts_bytes > (uint32_t)cfg_ts_max_kb * 1024) return;
    TSInput in = { b, ts_read, TSInputEncodingUTF8, NULL };
    /* ts_note_edit() has already applied every edit to the old tree, so this
     * reparse is incremental: cost tracks the size of the change, not the file */
    TSTree *t = ts_parser_parse(b->ts_parser, b->ts_tree, in);
    if (!t) return;
    if (b->ts_tree) ts_tree_delete(b->ts_tree);
    b->ts_tree = t;
    b->ts_ver = b->ver;
    ts_due = 0;
}
/* Collected captures for the visible window, shortest-span-last so that the
 * most specific capture is the one that ends up painted. */
typedef struct { uint32_t s, e; unsigned char a; } TSSpan;
static TSSpan *tsspan = NULL;
static int     ntsspan = 0, tsspan_cap = 0;
static int     tsspan_buf_ver = -1;
static Buf    *tsspan_buf = NULL;
static int     tsspan_y0 = -1, tsspan_y1 = -1;

/* Move the cached spans with an edit, so the colors from the last parse stay
 * on the text they belong to until the next one. */
void ts_shift_spans(Buf *b, uint32_t sb, uint32_t ob, uint32_t nb) {
    ts_edit_ms = ts_now_ms();
    if (tsspan_buf != b || ob == nb) return;
    long d = (long)nb - (long)ob;
    for (int i = 0; i < ntsspan; i++) {
        uint32_t s = tsspan[i].s, e = tsspan[i].e;
        tsspan[i].s = s >= ob ? (uint32_t)((long)s + d) : (s > sb ? sb : s);
        tsspan[i].e = e >= ob ? (uint32_t)((long)e + d) : (e > sb ? sb : e);
    }
}
/* a buffer is going away — make sure the span cache stops referring to it */
void ts_forget(Buf *b) {
    if (tsspan_buf == b) { tsspan_buf = NULL; ntsspan = 0; tsspan_buf_ver = -1; }
}
static int tsspan_cmp(const void *x, const void *y) {
    const TSSpan *a = x, *b = y;
    uint32_t la = a->e - a->s, lb = b->e - b->s;
    if (la != lb) return la < lb ? 1 : -1;      /* longest first */
    return a->s < b->s ? -1 : a->s > b->s;
}
/* Run the highlight query over lines [y0,y1] only. */
void ts_collect(Buf *b, int y0, int y1) {
    TSGram *g = ts_for(b->lang);
    if (!g) return;
    /* Parse when the typing stops, not on a timer: a run of keystrokes gets
     * one parse at the end of it, and the shifted spans carry the colors in
     * the meantime. */
    if (!b->ts_tree || b->ts_ver == b->ver ||
        ts_now_ms() - ts_edit_ms >= TS_DEBOUNCE_MS)
        ts_reparse(b);
    else
        ts_due = 1;
    if (!b->ts_tree) return;
    if (tsspan_buf == b && tsspan_buf_ver == b->ts_ver &&
        tsspan_y0 == y0 && tsspan_y1 == y1) return;         /* cache hit */

    ntsspan = 0;
    tsspan_buf = b; tsspan_buf_ver = b->ts_ver;
    tsspan_y0 = y0; tsspan_y1 = y1;

    static TSQueryCursor *cur_q = NULL;
    if (!cur_q) cur_q = ts_query_cursor_new();
    ts_query_cursor_set_byte_range(cur_q, b->ts_off[y0], b->ts_off[y1 + 1]);
    ts_query_cursor_exec(cur_q, g->query, ts_tree_root_node(b->ts_tree));

    TSQueryMatch m;
    while (ts_query_cursor_next_match(cur_q, &m)) {
        for (uint16_t i = 0; i < m.capture_count; i++) {
            unsigned char a = g->cap[m.captures[i].index];
            if (a == HA_DEF) continue;
            uint32_t s = ts_node_start_byte(m.captures[i].node);
            uint32_t e = ts_node_end_byte(m.captures[i].node);
            if (e <= s) continue;
            if (ntsspan == tsspan_cap) {
                tsspan_cap = tsspan_cap ? tsspan_cap * 2 : 256;
                tsspan = xrealloc(tsspan, (size_t)tsspan_cap * sizeof *tsspan);
            }
            tsspan[ntsspan].s = s; tsspan[ntsspan].e = e; tsspan[ntsspan].a = a;
            ntsspan++;
        }
    }
    qsort(tsspan, (size_t)ntsspan, sizeof *tsspan, tsspan_cmp);
}
/* Paint line `li`'s attrs from the collected spans. Returns 0 if tree-sitter
 * has nothing for this buffer and the caller should use the lexer. */
int ts_line_attrs(Buf *b, int li, unsigned char *attr, int limit) {
    if (!b->ts_tree || !tsspan || tsspan_buf != b) return 0;
    int len = min2(limit, b->ln[li].len);
    memset(attr, HA_DEF, (size_t)len);
    uint32_t ls = b->ts_off[li], le = ls + (uint32_t)len;
    for (int i = 0; i < ntsspan; i++) {
        if (tsspan[i].e <= ls || tsspan[i].s >= le) continue;
        uint32_t a = tsspan[i].s > ls ? tsspan[i].s - ls : 0;
        uint32_t z = tsspan[i].e < le ? tsspan[i].e - ls : (uint32_t)len;
        for (uint32_t k = a; k < z; k++) attr[k] = tsspan[i].a;
    }
    return 1;
}
#else
int ts_pending_ms(void) { return -1; }   /* no parser, nothing to wait for */
#endif /* SDS_TREESITTER */

/* ── main ─────────────────────────────────────────────────────────── */
/* Build a tree-sitter grammar from its upstream repo and install it, with the
 * matching highlight query, where ts_for() looks. Needs git and a compiler.
 * Grammars are not packaged consistently across distros — on Arch there is no
 * tree-sitter-cpp package at all — so sds ships this rather than assume. */
int fetch_grammar(const char *lang) {
#ifndef SDS_TREESITTER
    (void)lang;
    fprintf(stderr, "sds: built without tree-sitter support\n"
                    "     rebuild with -DSDS_TREESITTER (see install.sh)\n");
    return 1;
#else
    char gdir[PATH_MAX - 64], qdir[PATH_MAX - 64], tmp[256], cmd[PATH_MAX * 8];
    for (const char *p = lang; *p; p++)
        if (!isalnum((unsigned char)*p) && *p != '_') {
            fprintf(stderr, "sds: bad grammar name: %s\n", lang);
            return 1;
        }
    ts_data_dir(gdir, sizeof gdir, "grammars");
    ts_data_dir(qdir, sizeof qdir, "queries");
    snprintf(tmp, sizeof tmp, "/tmp/sds-grammar-%s.%d", lang, (int)getpid());

    snprintf(cmd, sizeof cmd,
        "set -e\n"
        "mkdir -p '%s' '%s/%s'\n"
        "rm -rf '%s'\n"
        "git clone --depth 1 -q https://github.com/tree-sitter/tree-sitter-%s '%s'\n"
        "cd '%s'\n"
        "src=src\n"
        "[ -d bindings ] || true\n"
        "cc -O2 -fPIC -shared -I \"$src\" -o '%s/libtree-sitter-%s.so' \\\n"
        "   \"$src\"/parser.c $([ -f \"$src/scanner.c\" ] && echo \"$src/scanner.c\")\n"
        "cp queries/highlights.scm '%s/%s/highlights.scm'\n"
        "cd / && rm -rf '%s'\n",
        gdir, qdir, lang, tmp, lang, tmp, tmp, gdir, lang, qdir, lang, tmp);

    printf("fetching tree-sitter-%s …\n", lang);
    int st = system(cmd);
    if (st != 0) {
        fprintf(stderr, "sds: could not install grammar for %s\n", lang);
        return 1;
    }
    printf("installed %s/libtree-sitter-%s.so\n", gdir, lang);
    printf("installed %s/%s/highlights.scm\n", qdir, lang);

    /* A language whose query inherits another needs that one's rules too,
     * or e.g. C++ loses all its primitive-type highlighting. */
    const char *par = ts_parent_of(lang);
    if (par) {
        char pq[PATH_MAX];
        snprintf(pq, sizeof pq, "%s/%s/highlights.scm", qdir, par);
        if (access(pq, F_OK) != 0) {
            printf("%s inherits %s — fetching its query too\n", lang, par);
            snprintf(cmd, sizeof cmd,
                "set -e\n"
                "mkdir -p '%s/%s'\n"
                "rm -rf '%s'\n"
                "git clone --depth 1 -q "
                "https://github.com/tree-sitter/tree-sitter-%s '%s'\n"
                "cp '%s/queries/highlights.scm' '%s/%s/highlights.scm'\n"
                "rm -rf '%s'\n",
                qdir, par, tmp, par, tmp, tmp, qdir, par, tmp);
            if (system(cmd) == 0) printf("installed %s/%s/highlights.scm\n", qdir, par);
            else fprintf(stderr, "sds: warning: could not fetch %s query\n", par);
        }
    }
    return 0;
#endif
}
