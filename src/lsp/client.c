#include "core/sds.h"
#include "lsp/lsp.h"
#include "lsp/lsp_internal.h"
#include "tree/tree.h"

/* ── language server processes ────────────────────────────────────────
 * One process per server command, shared by every buffer it serves — clangd
 * answers for both C and C++. It talks JSON-RPC over its stdin and stdout,
 * each message a "Content-Length: N" header block followed by N bytes.
 *
 * Both pipes are non-blocking. Outgoing messages queue in `w` and drain as
 * the server reads; incoming bytes collect in `r` until a whole message is
 * there. Everything happens from lsp_tick(), which the main loop calls on
 * its poll tick, so a slow or wedged server can never stall the editor.   */

enum { SRV_STARTING, SRV_READY, SRV_DEAD };

struct LspServer {
    char   cmd[256];
    pid_t  pid;
    int    in, out;            /* our ends: we write `in`, read `out` */
    int    state;
    JBuf   w;                  /* queued outgoing bytes */
    size_t woff;               /* how much of `w` has been written */
    JBuf   r;                  /* incoming bytes not yet parsed */
    int    next_id;
};

#define LSP_MAXSRV 4
static LspServer *srv[LSP_MAXSRV];
static int nsrv = 0;

long lsp_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
static void set_flags(int fd) {
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    /* terminal tabs fork shells; a shell holding a copy of the server's stdin
     * would keep the server alive after sds is gone */
    fcntl(fd, F_SETFD, FD_CLOEXEC);
}
static void queue(LspServer *s, JBuf *body) {
    char hdr[64];
    int hl = snprintf(hdr, sizeof hdr, "Content-Length: %zu\r\n\r\n", body->n);
    jb_raw(&s->w, hdr, (size_t)hl);
    jb_raw(&s->w, body->s, body->n);
}
static void request(LspServer *s, const char *method, JBuf *params) {
    JBuf m = { 0 };
    jb_cat(&m, "{\"jsonrpc\":\"2.0\",\"id\":");
    jb_int(&m, s->next_id++);
    jb_cat(&m, ",\"method\":");
    jb_str(&m, method, strlen(method));
    jb_cat(&m, ",\"params\":");
    if (params && params->n) jb_raw(&m, params->s, params->n);
    else jb_cat(&m, "null");
    jb_cat(&m, "}");
    queue(s, &m);
    free(m.s);
}
void lsp_notify(LspServer *s, const char *method, JBuf *params) {
    if (!s || s->state == SRV_DEAD) return;
    JBuf m = { 0 };
    jb_cat(&m, "{\"jsonrpc\":\"2.0\",\"method\":");
    jb_str(&m, method, strlen(method));
    jb_cat(&m, ",\"params\":");
    if (params && params->n) jb_raw(&m, params->s, params->n);
    else jb_cat(&m, "{}");
    jb_cat(&m, "}");
    queue(s, &m);
    free(m.s);
}
static void mark_dead(LspServer *s, const char *why) {
    if (s->state == SRV_DEAD) return;
    s->state = SRV_DEAD;
    if (s->in >= 0)  { close(s->in);  s->in = -1; }
    if (s->out >= 0) { close(s->out); s->out = -1; }
    if (s->pid > 0 && waitpid(s->pid, NULL, WNOHANG) == s->pid) s->pid = 0;
    if (why) set_msg("%s", why);
}

/* The URI clangd will send back is the one we send, so a plain file:// of the
 * absolute path is all that is needed; the root only has to be a real one. */
static void spawn(LspServer *s) {
    int to[2], from[2];
    if (pipe(to) != 0) { s->state = SRV_DEAD; return; }
    if (pipe(from) != 0) { close(to[0]); close(to[1]); s->state = SRV_DEAD; return; }
    /* A server that dies while we are writing to it would otherwise take sds
     * down with SIGPIPE; with it ignored the write just fails with EPIPE. */
    signal(SIGPIPE, SIG_IGN);

    char cwd[PATH_MAX];
    const char *rootdir = root ? root->path : (getcwd(cwd, sizeof cwd) ? cwd : "/");
    pid_t pid = fork();
    if (pid < 0) {
        close(to[0]); close(to[1]); close(from[0]); close(from[1]);
        s->state = SRV_DEAD;
        return;
    }
    if (pid == 0) {
        dup2(to[0], STDIN_FILENO);
        dup2(from[1], STDOUT_FILENO);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) dup2(devnull, STDERR_FILENO);
        close(to[0]); close(to[1]); close(from[0]); close(from[1]);
        if (chdir(rootdir) != 0) { /* the root URI still tells it where */ }
        /* through the shell, so the configured command may carry arguments */
        char line[300];
        snprintf(line, sizeof line, "exec %s", s->cmd);
        execl("/bin/sh", "sh", "-c", line, (char *)NULL);
        _exit(127);
    }
    close(to[0]);
    close(from[1]);
    s->pid = pid;
    s->in = to[1];
    s->out = from[0];
    set_flags(s->in);
    set_flags(s->out);
    s->state = SRV_STARTING;
    s->next_id = 1;

    JBuf p = { 0 };
    jb_cat(&p, "{\"processId\":");
    jb_int(&p, (long)getpid());
    jb_cat(&p, ",\"rootUri\":");
    char *uri = lsp_uri(rootdir);
    jb_str(&p, uri, strlen(uri));
    free(uri);
    /* Only what sds uses: diagnostics, with the version they belong to so a
     * set computed for stale text can be told apart from a current one. */
    jb_cat(&p, ",\"capabilities\":{\"textDocument\":{"
               "\"synchronization\":{\"dynamicRegistration\":false},"
               "\"publishDiagnostics\":{\"versionSupport\":true}}},"
               "\"clientInfo\":{\"name\":\"sds\"}}");
    request(s, "initialize", &p);            /* id 1 */
    free(p.s);
}

/* The server for a command, started on first use. NULL when the program is
 * not installed, which the caller reports once. */
LspServer *lsp_server_for(const char *cmd) {
    for (int i = 0; i < nsrv; i++)
        if (!strcmp(srv[i]->cmd, cmd)) return srv[i];
    if (nsrv == LSP_MAXSRV) return NULL;

    /* check the program exists before forking, so a missing clangd is a
     * clear message rather than a server that silently exits */
    char prog[256], found[PATH_MAX];
    snprintf(prog, sizeof prog, "%.*s", (int)strcspn(cmd, " \t"), cmd);
    int have = strchr(prog, '/') ? access(prog, X_OK) == 0
                                 : find_exec(prog, found, sizeof found);
    LspServer *s = calloc(1, sizeof *s);
    if (!s) die("out of memory");
    snprintf(s->cmd, sizeof s->cmd, "%s", cmd);
    s->in = s->out = -1;
    srv[nsrv++] = s;
    if (!have) {
        s->state = SRV_DEAD;
        set_msg("%s not found — install it to see compile errors as you type", prog);
        return s;
    }
    spawn(s);
    return s;
}
int lsp_server_ready(LspServer *s) { return s && s->state == SRV_READY; }
int lsp_server_dead(LspServer *s)  { return !s || s->state == SRV_DEAD; }

/* ── incoming ─────────────────────────────────────────────────────── */
static void on_message(LspServer *s, JSpan msg) {
    JSpan id = js_get(msg, "id"), method = js_get(msg, "method");
    if (!method.p) {                                      /* a response */
        if (js_int(id, 0) == 1 && s->state == SRV_STARTING) {
            if (js_get(msg, "error").p) {
                mark_dead(s, "language server refused to start");
                return;
            }
            s->state = SRV_READY;
            lsp_notify(s, "initialized", NULL);
            for (int i = 0; i < lsp_ndocs; i++)          /* files opened early */
                if (lsp_docs[i]->srv == s && !lsp_docs[i]->opened)
                    lsp_send_open(lsp_docs[i]);
        }
        return;
    }
    char *m = js_str(method);
    if (!m) return;
    if (id.p) {
        /* A request from the server. sds advertises nothing that invites
         * one, but a server may ask anyway, and an unanswered request can
         * leave it waiting — so always reply, with an empty result. */
        JBuf r = { 0 };
        jb_cat(&r, "{\"jsonrpc\":\"2.0\",\"id\":");
        jb_raw(&r, id.p, (size_t)(id.e - id.p));
        jb_cat(&r, ",\"result\":null}");
        queue(s, &r);
        free(r.s);
    } else if (!strcmp(m, "textDocument/publishDiagnostics")) {
        lsp_on_diagnostics(js_get(msg, "params"));
    }
    free(m);
}
/* Split `r` into whole messages. Returns how many were handled. */
static int parse_incoming(LspServer *s) {
    int handled = 0;
    size_t at = 0;
    for (;;) {
        char *base = s->r.s + at;
        size_t avail = s->r.n - at;
        char *hend = NULL;
        for (size_t i = 0; i + 3 < avail; i++)
            if (base[i] == '\r' && base[i+1] == '\n' && base[i+2] == '\r' && base[i+3] == '\n') {
                hend = base + i;
                break;
            }
        if (!hend) break;
        long len = -1;
        for (char *p = base; p < hend; ) {                /* header lines */
            char *eol = p;
            while (eol < hend && *eol != '\r') eol++;
            if (eol - p > 15 && !strncasecmp(p, "Content-Length:", 15))
                len = strtol(p + 15, NULL, 10);
            p = eol + 2;
        }
        size_t hdr = (size_t)(hend - base) + 4;
        if (len < 0) { at += hdr; continue; }             /* malformed: skip it */
        if (avail < hdr + (size_t)len) break;             /* body not all here */
        JSpan msg = { base + hdr, base + hdr + len };
        on_message(s, msg);
        handled++;
        at += hdr + (size_t)len;
        if (s->state == SRV_DEAD) break;
    }
    if (at && s->r.s) {
        memmove(s->r.s, s->r.s + at, s->r.n - at);
        s->r.n -= at;
    }
    return handled;
}
static int pump(LspServer *s) {
    if (s->state == SRV_DEAD) return 0;
    while (s->woff < s->w.n) {                            /* write what fits */
        ssize_t k = write(s->in, s->w.s + s->woff, s->w.n - s->woff);
        if (k > 0) { s->woff += (size_t)k; continue; }
        if (k < 0 && errno == EINTR) continue;
        if (k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        mark_dead(s, "language server stopped");
        return 1;
    }
    if (s->woff == s->w.n) s->w.n = s->woff = 0;

    int got = 0;
    for (int guard = 0; guard < 64; guard++) {            /* bounded: stay responsive */
        char buf[16384];
        ssize_t k = read(s->out, buf, sizeof buf);
        if (k > 0) { jb_raw(&s->r, buf, (size_t)k); got = 1; continue; }
        if (k < 0 && errno == EINTR) continue;
        if (k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        mark_dead(s, "language server exited");
        return 1;
    }
    return got ? parse_incoming(s) > 0 : 0;
}

/* ── the tick ─────────────────────────────────────────────────────── */
/* How long typing has to pause before the new text goes to the server.
 * Sending on every key would have clangd re-parsing mid-word; waiting too
 * long makes the errors feel detached from the typing. */
#define LSP_DEBOUNCE_MS 300

/* Called from the main loop's poll tick. Returns 1 when something on screen
 * may have changed (new diagnostics, a status message). */
int lsp_tick(void) {
    int changed = 0;
    long now = lsp_now_ms();
    for (int i = 0; i < lsp_ndocs; i++) {
        LspDoc *d = lsp_docs[i];
        if (d->b->ver != d->ver_seen) { d->ver_seen = d->b->ver; d->seen_ms = now; }
        if (d->opened && d->ver_seen != d->ver_sent &&
            now - d->seen_ms >= LSP_DEBOUNCE_MS && lsp_server_ready(d->srv))
            lsp_send_change(d);
    }
    for (int i = 0; i < nsrv; i++) {
        changed |= pump(srv[i]);
        /* a server that closed its pipes may not have been reapable yet */
        if (srv[i]->state == SRV_DEAD && srv[i]->pid > 0 &&
            waitpid(srv[i]->pid, NULL, WNOHANG) == srv[i]->pid)
            srv[i]->pid = 0;
    }
    return changed;
}
/* How long the main loop may wait for a key, in milliseconds, or -1 to block.
 * Replies arrive on the server's schedule rather than in answer to a key, so
 * the loop has to look. It only needs to look often while an answer is
 * actually outstanding; otherwise a slow tick is enough to pick up a late
 * publish (clangd re-sends when its index catches up) without sds sitting in
 * a busy poll for the rest of the session. */
int lsp_poll_ms(void) {
    int alive = 0;
    for (int i = 0; i < nsrv; i++) {
        if (srv[i]->state == SRV_DEAD) continue;
        alive = 1;
        if (srv[i]->state == SRV_STARTING || srv[i]->woff < srv[i]->w.n) return 50;
    }
    if (!alive) return -1;
    for (int i = 0; i < lsp_ndocs; i++) {
        LspDoc *d = lsp_docs[i];
        if (lsp_server_dead(d->srv)) continue;
        if (!d->opened || !d->fresh || d->ver_seen != d->ver_sent) return 50;
    }
    return 500;
}
/* Ask each server to stop, then make sure it does. A server also exits on
 * its own when its stdin closes, so this only has to be polite, not
 * thorough. */
void lsp_shutdown(void) {
    for (int i = 0; i < nsrv; i++) {
        LspServer *s = srv[i];
        if (s->state == SRV_DEAD) continue;
        request(s, "shutdown", NULL);
        lsp_notify(s, "exit", NULL);
        fcntl(s->in, F_SETFL, fcntl(s->in, F_GETFL) & ~O_NONBLOCK);
        for (long end = lsp_now_ms() + 300; s->woff < s->w.n && lsp_now_ms() < end; ) {
            ssize_t k = write(s->in, s->w.s + s->woff, s->w.n - s->woff);
            if (k <= 0) break;
            s->woff += (size_t)k;
        }
        pid_t pid = s->pid;
        mark_dead(s, NULL);
        if (pid > 0) {
            long end = lsp_now_ms() + 300;
            while (waitpid(pid, NULL, WNOHANG) == 0 && lsp_now_ms() < end) {
                struct timespec ts = { 0, 10 * 1000000L };
                nanosleep(&ts, NULL);
            }
            if (waitpid(pid, NULL, WNOHANG) == 0) {
                kill(pid, SIGTERM);
                waitpid(pid, NULL, 0);
            }
        }
    }
}
