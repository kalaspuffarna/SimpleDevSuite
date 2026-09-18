#include "core/sds.h"
#include "editor/editor.h"
#include "editor/editor_internal.h"

char *clip = NULL;      int cliplen = 0;

/* ── clipboard ────────────────────────────────────────────────────── */
static void osc52_copy(const char *t, int len) {
    static const char b64[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if (len > 100000) return;
    char *out = xmalloc((size_t)len * 4 / 3 + 64);
    int n = 0;
    n += sprintf(out, "\033]52;c;");
    for (int i = 0; i < len; i += 3) {
        unsigned v = (unsigned char)t[i] << 16;
        if (i + 1 < len) v |= (unsigned char)t[i+1] << 8;
        if (i + 2 < len) v |= (unsigned char)t[i+2];
        out[n++] = b64[v >> 18 & 63];
        out[n++] = b64[v >> 12 & 63];
        out[n++] = i + 1 < len ? b64[v >> 6 & 63] : '=';
        out[n++] = i + 2 < len ? b64[v & 63] : '=';
    }
    out[n++] = '\a';
    ssize_t ignored = write(STDOUT_FILENO, out, (size_t)n);
    (void)ignored;
    free(out);
}
void clip_set(char *t, int len) {       /* takes ownership */
    free(clip);
    clip = t; cliplen = len;
    osc52_copy(t, len);
}
