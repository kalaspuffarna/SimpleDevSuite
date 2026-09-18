#include "core/sds.h"
#include "pdf/pdf.h"
#include "pdf/pdf_internal.h"

/* ── rendering a page as an image ─────────────────────────────────────
 * Terminals that speak the kitty graphics protocol (kitty, ghostty, WezTerm,
 * iTerm2) can show the real page instead of extracted text. sds does not
 * rasterize PDFs itself — that is a font engine and a path renderer, far more
 * than this file should carry — so it shells out to whichever of mutool,
 * pdftoppm or gs is installed, and says so plainly when none is.           */

enum { RAST_NONE = 0, RAST_MUTOOL, RAST_PDFTOPPM, RAST_GS };
static int  rast_kind = -1;          /* resolved on first use */
static char rast_path[PATH_MAX];
static int  gfx_ok = -1;             /* terminal speaks the protocol */

static int rast_find(void) {
    if (rast_kind >= 0) return rast_kind;
    if      (find_exec("mutool",   rast_path, sizeof rast_path)) rast_kind = RAST_MUTOOL;
    else if (find_exec("pdftoppm", rast_path, sizeof rast_path)) rast_kind = RAST_PDFTOPPM;
    else if (find_exec("gs",       rast_path, sizeof rast_path)) rast_kind = RAST_GS;
    else rast_kind = RAST_NONE;
    return rast_kind;
}
/* kitty, ghostty, WezTerm and iTerm2 all implement the protocol. There is a
 * query handshake for this, but it means reading a reply mid-startup; the
 * environment is reliable enough, and `render` in the config forces it. */
static int gfx_detect(void) {
    if (gfx_ok >= 0) return gfx_ok;
    const char *t = getenv("TERM"), *p = getenv("TERM_PROGRAM");
    gfx_ok = 0;
    if (getenv("KITTY_WINDOW_ID")) gfx_ok = 1;
    if (t && (strstr(t, "kitty") || strstr(t, "ghostty"))) gfx_ok = 1;
    if (p && (!strcasecmp(p, "ghostty") || !strcasecmp(p, "WezTerm") ||
              !strcasecmp(p, "iTerm.app"))) gfx_ok = 1;
    return gfx_ok;
}
/* Why a page cannot be shown as an image, or NULL when it can. */
const char *pdf_render_why_not(void) {
    if (!cfg_pdf_render) return "page rendering is off in the config";
    if (!gfx_detect())
        return "this terminal can't show images — needs kitty, ghostty or WezTerm";
    if (rast_find() == RAST_NONE)
        return "no PDF rasterizer found — install mupdf-tools, poppler or ghostscript";
    return NULL;
}
/* Terminal cell size in pixels, so a page can be rasterized to fit exactly. */
void cell_px(int *cw, int *ch) {
    struct winsize ws;
    *cw = 8; *ch = 16;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 &&
        ws.ws_xpixel > 0 && ws.ws_ypixel > 0 && ws.ws_col > 0 && ws.ws_row > 0) {
        int a = ws.ws_xpixel / ws.ws_col, b = ws.ws_ypixel / ws.ws_row;
        if (a > 0 && b > 0) { *cw = a; *ch = b; }
    }
}
/* Start rasterizing one page to `out` (a PNG) `W` pixels wide; the height
 * follows from the page's own aspect ratio. Returns the child's pid, or -1.
 * Rendering a page costs hundreds of milliseconds — half a second is normal
 * for a dense one — so the caller can let it run while sds carries on and
 * collect it with pdf_raster_done(). */
pid_t pdf_raster_start(const char *pdf, int page1, int W,
                       double pt_w, const char *out) {
    char sw[32], sp[32], sr[32], first[48], last[48], dev[64], sout[PATH_MAX + 16];
    char *argv[16];
    int n = 0;
    snprintf(sw, sizeof sw, "%d", W);
    snprintf(sp, sizeof sp, "%d", page1);
    switch (rast_find()) {
        case RAST_MUTOOL:
            argv[n++] = rast_path; argv[n++] = (char *)"draw";
            argv[n++] = (char *)"-q";
            argv[n++] = (char *)"-F"; argv[n++] = (char *)"png";
            argv[n++] = (char *)"-o"; argv[n++] = (char *)out;
            argv[n++] = (char *)"-w"; argv[n++] = sw;
            argv[n++] = (char *)pdf; argv[n++] = sp;
            break;
        case RAST_PDFTOPPM: {
            /* pdftoppm appends ".png" to the prefix it is given */
            size_t l = strlen(out);
            snprintf(sout, sizeof sout, "%.*s", (int)(l > 4 ? l - 4 : l), out);
            argv[n++] = rast_path; argv[n++] = (char *)"-png";
            argv[n++] = (char *)"-f"; argv[n++] = sp;
            argv[n++] = (char *)"-l"; argv[n++] = sp;
            argv[n++] = (char *)"-singlefile";
            argv[n++] = (char *)"-scale-to-x"; argv[n++] = sw;
            argv[n++] = (char *)"-scale-to-y"; argv[n++] = (char *)"-1";
            argv[n++] = (char *)pdf; argv[n++] = sout;
            break;
        }
        case RAST_GS:
            snprintf(sr, sizeof sr, "-r%d", pt_w > 1 ? (int)(W * 72.0 / pt_w + 0.5) : 96);
            snprintf(first, sizeof first, "-dFirstPage=%d", page1);
            snprintf(last, sizeof last, "-dLastPage=%d", page1);
            snprintf(dev, sizeof dev, "-sDEVICE=png16m");
            snprintf(sout, sizeof sout, "-sOutputFile=%s", out);
            argv[n++] = rast_path; argv[n++] = (char *)"-q";
            argv[n++] = (char *)"-dNOPAUSE"; argv[n++] = (char *)"-dBATCH";
            argv[n++] = (char *)"-dSAFER"; argv[n++] = dev;
            argv[n++] = sr; argv[n++] = first; argv[n++] = last;
            argv[n++] = sout; argv[n++] = (char *)pdf;
            break;
        default: return -1;
    }
    argv[n] = NULL;
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > 2) close(devnull);
        }
        execv(argv[0], argv);
        _exit(127);
    }
    return pid;
}
/* 1 = finished and the file is there, 0 = still going, -1 = it failed.
 * `wait` blocks until it is done. */
int pdf_raster_done(pid_t pid, const char *out, int wait) {
    int st = 0;
    pid_t r;
    while ((r = waitpid(pid, &st, wait ? 0 : WNOHANG)) < 0 && errno == EINTR) { }
    if (r == 0) return 0;
    if (r != pid || !WIFEXITED(st) || WEXITSTATUS(st) != 0) return -1;
    struct stat sb;
    if (stat(out, &sb) != 0 || sb.st_size == 0) return -1;
    return 1;
}
int pdf_raster(const char *pdf, int page1, int W, double pt_w, const char *out) {
    pid_t pid = pdf_raster_start(pdf, page1, W, pt_w, out);
    if (pid < 0) return -1;
    return pdf_raster_done(pid, out, 1) == 1 ? 0 : -1;
}
