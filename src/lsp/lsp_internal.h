/* lsp_internal.h — language server client (shared between the files in lsp/ only) */
#ifndef SDS_LSP_INTERNAL_H
#define SDS_LSP_INTERNAL_H

#include "core/sds.h"
#include "lsp/lsp.h"

/* json.c */
/* A JSON value as a byte range into the message it came from; nothing is
 * copied until a string is actually wanted. p == NULL means "absent". */
typedef struct { const char *p, *e; } JSpan;

JSpan js_get(JSpan obj, const char *key);
int   js_next(const char **p, const char *e, JSpan *out);
const char *js_open(JSpan v, char brace);
long  js_int(JSpan v, long dflt);
char *js_str(JSpan v);
int   js_is(JSpan v, const char *lit);

/* A growable output buffer, for building messages. */
typedef struct { char *s; size_t n, cap; } JBuf;

void jb_raw(JBuf *b, const char *s, size_t n);
void jb_cat(JBuf *b, const char *s);
void jb_str(JBuf *b, const char *s, size_t n);
void jb_int(JBuf *b, long v);

/* client.c */
typedef struct LspServer LspServer;

struct LspDoc {
    Buf       *b;
    LspServer *srv;
    char      *uri;
    const char *lang_id;       /* "c", "cpp" */
    int        opened;         /* didOpen has gone out */
    int        ver_sent;       /* b->ver the server last saw */
    int        ver_seen;       /* b->ver at the last tick, for the debounce */
    long       seen_ms;        /* when ver_seen last changed */
    int        fresh;          /* diagnostics received for ver_sent */
    Diag      *d;              /* sorted by line, then column */
    int        nd;
};

LspServer *lsp_server_for(const char *cmd);
int   lsp_server_ready(LspServer *s);
int   lsp_server_dead(LspServer *s);
void  lsp_notify(LspServer *s, const char *method, JBuf *params);

/* docs.c */
char *lsp_uri(const char *path);
extern LspDoc *lsp_docs[MAX_TABS];
extern int     lsp_ndocs;

void lsp_send_open(LspDoc *d);
void lsp_send_change(LspDoc *d);
void lsp_on_diagnostics(JSpan params);

#endif /* SDS_LSP_INTERNAL_H */
