#include "core/sds.h"
#include "lsp/lsp.h"
#include "lsp/lsp_internal.h"

/* ── reading ──────────────────────────────────────────────────────────
 * Just enough JSON for the messages a language server sends: walk to a key,
 * step through an array, read a number or a string. Values are spans into
 * the message buffer, the same approach the PDF parser takes, so a large
 * reply is never turned into a tree only to read three fields out of it.  */

static const char *js_ws(const char *p, const char *e) {
    while (p < e && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    return p;
}
/* Past one value of any kind. Returns p itself on garbage, so a caller's loop
 * cannot spin: every caller treats "no progress" as the end. */
static const char *js_skip(const char *p, const char *e) {
    p = js_ws(p, e);
    if (p >= e) return p;
    if (*p == '"') {
        for (p++; p < e; p++) {
            if (*p == '\\') { p++; continue; }
            if (*p == '"') return p + 1;
        }
        return e;
    }
    if (*p == '{' || *p == '[') {
        int depth = 0;
        for (; p < e; p++) {
            if (*p == '"') { p = js_skip(p, e) - 1; continue; }
            if (*p == '{' || *p == '[') depth++;
            else if (*p == '}' || *p == ']') { if (--depth == 0) return p + 1; }
        }
        return e;
    }
    const char *s = p;             /* number, true, false, null */
    while (p < e && *p != ',' && *p != '}' && *p != ']' &&
           *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') p++;
    return p > s ? p : s;
}
/* Inside of an object or array: the first byte after its opening brace, or
 * NULL when `v` is not that kind of value. */
const char *js_open(JSpan v, char brace) {
    if (!v.p) return NULL;
    const char *p = js_ws(v.p, v.e);
    return (p < v.e && *p == brace) ? p + 1 : NULL;
}
/* Next element of an array (or next "key": value pair's value, when used on
 * an object's members after js_get has positioned p). */
int js_next(const char **pp, const char *e, JSpan *out) {
    const char *p = js_ws(*pp, e);
    if (p < e && *p == ',') p = js_ws(p + 1, e);
    if (p >= e || *p == ']' || *p == '}') return 0;
    const char *q = js_skip(p, e);
    if (q <= p) return 0;
    out->p = p;
    out->e = q;
    *pp = q;
    return 1;
}
JSpan js_get(JSpan obj, const char *key) {
    JSpan none = { NULL, NULL };
    const char *p = js_open(obj, '{');
    if (!p) return none;
    size_t kl = strlen(key);
    for (;;) {
        p = js_ws(p, obj.e);
        if (p < obj.e && *p == ',') p = js_ws(p + 1, obj.e);
        if (p >= obj.e || *p != '"') return none;
        const char *ks = p + 1, *ke = js_skip(p, obj.e);
        if (ke <= p) return none;
        p = js_ws(ke, obj.e);
        if (p >= obj.e || *p != ':') return none;
        const char *vs = js_ws(p + 1, obj.e), *ve = js_skip(vs, obj.e);
        if (ve <= vs) return none;
        /* keys are compared raw: the ones LSP uses never need escaping */
        if ((size_t)(ke - 1 - ks) == kl && memcmp(ks, key, kl) == 0) {
            JSpan v = { vs, ve };
            return v;
        }
        p = ve;
    }
}
long js_int(JSpan v, long dflt) {
    if (!v.p) return dflt;
    const char *p = js_ws(v.p, v.e);
    if (p >= v.e || !(isdigit((unsigned char)*p) || *p == '-')) return dflt;
    return strtol(p, NULL, 10);
}
int js_is(JSpan v, const char *lit) {
    if (!v.p) return 0;
    size_t n = strlen(lit);
    return (size_t)(v.e - v.p) == n && memcmp(v.p, lit, n) == 0;
}
static int js_hex4(const char *p, const char *e) {
    if (e - p < 4) return -1;
    int v = 0;
    for (int i = 0; i < 4; i++) {
        int c = (unsigned char)p[i], d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return -1;
        v = v * 16 + d;
    }
    return v;
}
/* A string value, unescaped, as UTF-8 in a new allocation. NULL if `v` is
 * not a string. */
char *js_str(JSpan v) {
    if (!v.p) return NULL;
    const char *p = js_ws(v.p, v.e), *e = v.e;
    if (p >= e || *p != '"') return NULL;
    char *out = xmalloc((size_t)(e - p) + 1);
    size_t n = 0;
    for (p++; p < e && *p != '"'; p++) {
        if (*p != '\\') { out[n++] = *p; continue; }
        if (++p >= e) break;
        switch (*p) {
            case 'n': out[n++] = '\n'; break;
            case 't': out[n++] = '\t'; break;
            case 'r': out[n++] = '\r'; break;
            case 'b': out[n++] = '\b'; break;
            case 'f': out[n++] = '\f'; break;
            case 'u': {
                int u = js_hex4(p + 1, e);
                if (u < 0) break;
                p += 4;
                /* a surrogate pair is one code point spelled as two escapes */
                if (u >= 0xd800 && u < 0xdc00 && p + 6 < e && p[1] == '\\' && p[2] == 'u') {
                    int lo = js_hex4(p + 3, e);
                    if (lo >= 0xdc00 && lo < 0xe000) {
                        u = 0x10000 + ((u - 0xd800) << 10) + (lo - 0xdc00);
                        p += 6;
                    }
                }
                /* \u escapes are at most 3 bytes longer as UTF-8 than the 6
                 * they replace, and pairs shrink, so `out` is big enough */
                char tmp[8];
                int k = 0;
                if (u < 0x80) tmp[k++] = (char)u;
                else if (u < 0x800) {
                    tmp[k++] = (char)(0xc0 | u >> 6);
                    tmp[k++] = (char)(0x80 | (u & 0x3f));
                } else if (u < 0x10000) {
                    tmp[k++] = (char)(0xe0 | u >> 12);
                    tmp[k++] = (char)(0x80 | (u >> 6 & 0x3f));
                    tmp[k++] = (char)(0x80 | (u & 0x3f));
                } else {
                    tmp[k++] = (char)(0xf0 | u >> 18);
                    tmp[k++] = (char)(0x80 | (u >> 12 & 0x3f));
                    tmp[k++] = (char)(0x80 | (u >> 6 & 0x3f));
                    tmp[k++] = (char)(0x80 | (u & 0x3f));
                }
                memcpy(out + n, tmp, (size_t)k);
                n += (size_t)k;
                break;
            }
            default: out[n++] = *p;             /* \" \\ \/ */
        }
    }
    out[n] = 0;
    return out;
}

/* ── writing ──────────────────────────────────────────────────────── */
void jb_raw(JBuf *b, const char *s, size_t n) {
    if (b->n + n + 1 > b->cap) {
        b->cap = (b->n + n + 1) * 2;
        b->s = xrealloc(b->s, b->cap);
    }
    memcpy(b->s + b->n, s, n);
    b->n += n;
    b->s[b->n] = 0;
}
void jb_cat(JBuf *b, const char *s) { jb_raw(b, s, strlen(s)); }
void jb_int(JBuf *b, long v) {
    char t[32];
    jb_raw(b, t, (size_t)snprintf(t, sizeof t, "%ld", v));
}
/* `s` as a quoted JSON string. Bytes >= 0x80 pass through: the text is UTF-8
 * already, which is what the protocol wants on the wire. */
void jb_str(JBuf *b, const char *s, size_t n) {
    static const char hex[] = "0123456789abcdef";
    jb_raw(b, "\"", 1);
    size_t run = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 0x20 && c != '"' && c != '\\') { run++; continue; }
        jb_raw(b, s + i - run, run);
        run = 0;
        char esc[7] = { '\\', 0 };
        size_t k = 2;
        switch (c) {
            case '"':  esc[1] = '"';  break;
            case '\\': esc[1] = '\\'; break;
            case '\n': esc[1] = 'n';  break;
            case '\t': esc[1] = 't';  break;
            case '\r': esc[1] = 'r';  break;
            default:
                esc[1] = 'u'; esc[2] = '0'; esc[3] = '0';
                esc[4] = hex[c >> 4]; esc[5] = hex[c & 15];
                k = 6;
        }
        jb_raw(b, esc, k);
    }
    jb_raw(b, s + n - run, run);
    jb_raw(b, "\"", 1);
}
