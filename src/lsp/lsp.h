/* lsp.h — language server client: live compile diagnostics */
#ifndef SDS_LSP_H
#define SDS_LSP_H

#include "core/sds.h"

/* config [lsp] */
extern int  cfg_lsp_enabled;
extern char cfg_clangd[256];

enum { DIAG_ERROR = 1, DIAG_WARNING, DIAG_INFO, DIAG_HINT };

/* One diagnostic, positioned in the buffer as it stands: `line` is kept in
 * step with edits until the server publishes a fresh set. `col` is a byte
 * index into that line. */
typedef struct {
    int   line, col;
    int   severity;
    char *msg;
} Diag;

/* docs.c */
void lsp_attach(Buf *b);
void lsp_detach(Buf *b);
int  lsp_line_diags(Buf *b, int li, const Diag **first);
int  lsp_doc_state(Buf *b);
void lsp_lines_inserted(Buf *b, int at, int n);
void lsp_lines_deleted(Buf *b, int y1, int x1, int y2, int x2);

/* client.c */
int  lsp_tick(void);
int  lsp_poll_ms(void);
void lsp_shutdown(void);
long lsp_now_ms(void);

#endif /* SDS_LSP_H */
