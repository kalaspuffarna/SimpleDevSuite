#include "core/sds.h"
#include "pdf/pdf.h"
#include "pdf/pdf_internal.h"

/* ── kitty graphics protocol ──────────────────────────────────────── */
static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void kitty_delete(int id) {                  /* image and its data */
    printf("\033_Ga=d,d=I,i=%d\033\\", id);
}
/* Drop one placement, keeping the image itself: `place` distinguishes two
 * panes showing the same page, which share the image but not the spot. */
static void kitty_unplace(int id, int place) {
    printf("\033_Ga=d,d=i,i=%d,p=%d\033\\", id, place);
}
/* Width and height out of a PNG's IHDR chunk, which sits at a fixed offset.
 * Returns 0 on success. */
static int png_size(const char *path, int *w, int *h) {
    unsigned char hd[24];
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t got = fread(hd, 1, sizeof hd, f);
    fclose(f);
    if (got != sizeof hd || memcmp(hd, "\x89PNG\r\n\x1a\n", 8) != 0) return -1;
    unsigned iw = ((unsigned)hd[16] << 24) | ((unsigned)hd[17] << 16) |
                  ((unsigned)hd[18] << 8) | hd[19];
    unsigned ih = ((unsigned)hd[20] << 24) | ((unsigned)hd[21] << 16) |
                  ((unsigned)hd[22] << 8) | hd[23];
    if (!iw || !ih || iw >= (1u << 20) || ih >= (1u << 20)) return -1;
    *w = (int)iw;
    *h = (int)ih;
    return 0;
}
/* Transmit a PNG as image `id` without displaying it: the page is rendered
 * larger than the pane, and scrolling only re-places the crop rather than
 * pushing the whole picture down the wire again. The data goes inline in 4KB
 * base64 chunks, which every implementation of the protocol accepts (file
 * transmission is not universally supported). */
static int kitty_transmit(const char *png, int id) {
    FILE *f = fopen(png, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long sz = ftell(f);
    if (sz <= 0 || sz > (64L << 20)) { fclose(f); return -1; }
    rewind(f);
    unsigned char *raw = xmalloc((size_t)sz);
    if (fread(raw, 1, (size_t)sz, f) != (size_t)sz) { free(raw); fclose(f); return -1; }
    fclose(f);

    size_t b64n = ((size_t)sz + 2) / 3 * 4;
    char *b64 = xmalloc(b64n + 4);
    size_t o = 0;
    for (long i = 0; i < sz; i += 3) {
        unsigned v = (unsigned)raw[i] << 16;
        int rem = (int)(sz - i);
        if (rem > 1) v |= (unsigned)raw[i + 1] << 8;
        if (rem > 2) v |= raw[i + 2];
        b64[o++] = B64[(v >> 18) & 63];
        b64[o++] = B64[(v >> 12) & 63];
        b64[o++] = rem > 1 ? B64[(v >> 6) & 63] : '=';
        b64[o++] = rem > 2 ? B64[v & 63] : '=';
    }
    free(raw);

    const size_t CH = 4096;
    for (size_t p = 0; p < o; p += CH) {
        size_t n = min2((int)CH, (int)(o - p));
        int more = (p + n < o);
        if (p == 0) printf("\033_Gf=100,a=t,q=2,i=%d,m=%d;", id, more);
        else        printf("\033_Gm=%d;", more);
        fwrite(b64 + p, 1, n, stdout);
        fputs("\033\\", stdout);
    }
    free(b64);
    return 0;
}
/* Show the (sx,sy,sw,sh) rectangle of image `id` in a cols x rows box whose
 * top-left cell is (y,x). The source rect is a whole number of cells wide and
 * tall wherever the image is bigger than the pane, so nothing is rescaled. */
static void kitty_put(int id, int place, int y, int x, int cols, int rows,
                      int sx, int sy, int sw, int sh) {
    printf("\033[%d;%dH", y + 1, x + 1);          /* cursor to the pane corner */
    printf("\033_Ga=p,q=2,i=%d,p=%d,c=%d,r=%d,x=%d,y=%d,w=%d,h=%d\033\\",
           id, place, cols, rows, sx, sy, sw, sh);
}
/* What each pane wants on screen this frame; emitted after ncurses refreshes,
 * because the escapes must not be overwritten by a curses update.
 *
 * Rendering a page is the expensive part of this viewer — a few hundred
 * milliseconds of pdftoppm or gs, and more for a dense page — so a rendered
 * page is kept rather than thrown away: the file stays on disk, the terminal
 * keeps the pixels under its image id, and coming back to a page is a
 * placement escape. While the reader is on one page the next one is rendered
 * in the background, so turning forward usually finds it already there. */
#define GFX_ID_BASE 7100
#define PDF_RASTER_MAXW 6000            /* keeps a deep zoom from eating memory */
#define GFX_CACHE 6                     /* pages kept rendered */

typedef struct {
    char sig[PATH_MAX + 64];            /* path|page|width — what it holds */
    char png[PATH_MAX];
    int  iw, ih;                        /* size of the rendered page */
    int  live;                          /* the terminal holds it under its id */
    long used;                          /* for picking what to drop */
} GfxPage;
static GfxPage gcache[GFX_CACHE];
static long gfx_clock;

static struct {
    int  want;
    int  slot;                          /* which cache entry this pane shows */
    int  y, x, cols, rows;              /* where the crop lands, in cells */
    int  sx, sy, sw, sh;                /* the crop itself, in image pixels */
    char place[96];                     /* identifies where it is shown */
} gfx[MAX_PANES];
static int  gfx_live_slot[MAX_PANES] = { -1, -1, -1, -1 };  /* slot per pane */
static char gfx_place_live[MAX_PANES][96];

/* what the reader is looking at, so the next page can be got ready */
static struct {
    char path[PATH_MAX];
    int  page, npg, width;
    double pt_w;
} gfx_at;
static pid_t gfx_pre_pid = -1;          /* the background render, if any */
static char  gfx_pre_sig[PATH_MAX + 64];
static char  gfx_pre_png[PATH_MAX];

static void gfx_drop(int slot) {
    if (gcache[slot].live) kitty_delete(GFX_ID_BASE + slot);
    if (gcache[slot].png[0]) unlink(gcache[slot].png);
    memset(&gcache[slot], 0, sizeof gcache[slot]);
    for (int i = 0; i < MAX_PANES; i++)
        if (gfx_live_slot[i] == slot) { gfx_live_slot[i] = -1; gfx_place_live[i][0] = 0; }
}
static int gfx_find(const char *sig) {
    for (int i = 0; i < GFX_CACHE; i++)
        if (gcache[i].sig[0] && !strcmp(gcache[i].sig, sig)) return i;
    return -1;
}
/* A free slot, or the least recently used one — never one a pane is showing
 * this frame, or the page on screen would vanish. */
static int gfx_slot_for(const char *sig) {
    int hit = gfx_find(sig);
    if (hit >= 0) return hit;
    int best = -1;
    for (int i = 0; i < GFX_CACHE; i++) {
        int in_use = 0;
        for (int p = 0; p < MAX_PANES; p++)
            if (gfx[p].want && gfx[p].slot == i) in_use = 1;
        if (in_use) continue;
        if (!gcache[i].sig[0]) { best = i; break; }
        if (best < 0 || gcache[i].used < gcache[best].used) best = i;
    }
    if (best < 0) return -1;
    gfx_drop(best);
    return best;
}
static void gfx_tmp_path(char *out, size_t cap, int slot) {
    const char *dir = getenv("XDG_RUNTIME_DIR");
    if (!dir || !*dir) dir = "/tmp";
    snprintf(out, cap, "%s/sds-%d-%d.png", dir, (int)getpid(), slot);
}
void gfx_reset_frame(void) {
    for (int i = 0; i < MAX_PANES; i++) gfx[i].want = 0;
}
/* Rasterize if needed and work out which part of the page this pane shows.
 * The page is rendered `zoom` times as wide as the pane and scrolled inside,
 * rather than squeezed in whole: a page shrunk to fit a half-width pane is
 * too small to read, and that is the size this viewer is mostly used at. */
void gfx_request(int pane, Buf *b, int y, int x, int cols, int rows) {
    if (pane < 0 || pane >= MAX_PANES || cols < 2 || rows < 2) return;
    int cw, chh;
    cell_px(&cw, &chh);
    int W = cols * cw, H = rows * chh;
    if (W < 16 || H < 16) return;
    Pdf *p = b->pdf;
    if (!(p->zoom >= PDF_ZOOM_MIN && p->zoom <= PDF_ZOOM_MAX)) p->zoom = 1.0;

    int RW = (int)(W * p->zoom + 0.5);
    RW = max2(32, min2(PDF_RASTER_MAXW, RW));
    char sig[PATH_MAX + 64];
    snprintf(sig, sizeof sig, "%s|%d|w%d", b->path, p->page, RW);

    double pt_w = 612;
    if (p->npg > 0) {
        PdfPage *g = &p->pg[p->page];
        if (g->mb[2] - g->mb[0] > 1) pt_w = g->mb[2] - g->mb[0];
    }
    /* remember what is being read, so gfx_tick can get the next page ready */
    snprintf(gfx_at.path, sizeof gfx_at.path, "%s", b->path);
    gfx_at.page = p->page;
    gfx_at.npg = p->npg;
    gfx_at.width = RW;
    gfx_at.pt_w = pt_w;

    gfx[pane].want = 1;
    int slot = gfx_slot_for(sig);
    if (slot < 0) { gfx[pane].want = 0; return; }   /* every slot is on screen */
    gfx[pane].slot = slot;

    if (!gcache[slot].sig[0]) {                     /* not rendered yet */
        char tmp[PATH_MAX];
        gfx_tmp_path(tmp, sizeof tmp, slot);
        /* the background render may already have this very page waiting */
        int got = 0;
        if (gfx_pre_pid > 0 && !strcmp(gfx_pre_sig, sig) &&
            pdf_raster_done(gfx_pre_pid, gfx_pre_png, 1) == 1) {
            got = rename(gfx_pre_png, tmp) == 0;
            gfx_pre_pid = -1;
            gfx_pre_sig[0] = 0;
        }
        if (!got) got = pdf_raster(b->path, p->page + 1, RW, pt_w, tmp) == 0;
        if (!got || png_size(tmp, &gcache[slot].iw, &gcache[slot].ih) != 0) {
            unlink(tmp);
            gfx[pane].want = 0;
            b->pdf_img = 0;                   /* fall back to text next frame */
            set_msg("could not render this page — showing text%s", "");
            return;
        }
        snprintf(gcache[slot].sig, sizeof gcache[slot].sig, "%s", sig);
        snprintf(gcache[slot].png, sizeof gcache[slot].png, "%s", tmp);
    }
    gcache[slot].used = ++gfx_clock;
    int iw = gcache[slot].iw, ih = gcache[slot].ih;
    if (iw <= 0 || ih <= 0) { gfx[pane].want = 0; return; }

    /* The rendered page is the authority on how far it can scroll, so the
     * clamp lives here rather than in the key handler. */
    p->sx = max2(0, min2(p->sx, iw - W));
    p->sy = max2(0, min2(p->sy, ih - H));
    p->img_w = iw; p->img_h = ih;
    p->view_w = W; p->view_h = H;

    int sw = min2(iw, W), sh = min2(ih, H);
    int c = max2(1, min2(cols, (sw + cw - 1) / cw));
    int r = max2(1, min2(rows, (sh + chh - 1) / chh));
    gfx[pane].y = y;
    gfx[pane].x = x + (cols - c) / 2;         /* centre a page narrower than the pane */
    gfx[pane].cols = c;  gfx[pane].rows = r;
    gfx[pane].sx = p->sx; gfx[pane].sy = p->sy;
    gfx[pane].sw = sw;    gfx[pane].sh = sh;
    snprintf(gfx[pane].place, sizeof gfx[pane].place, "%d,%d,%d,%d,%d,%d,%d,%d",
             gfx[pane].y, gfx[pane].x, c, r, p->sx, p->sy, sw, sh);
}
/* Called from the main loop when nothing else is happening: collect a
 * finished background render, or start one for the page after this one. */
void gfx_tick(void) {
    if (gfx_pre_pid > 0) {
        int st = pdf_raster_done(gfx_pre_pid, gfx_pre_png, 0);
        if (st == 0) return;                  /* still rendering */
        gfx_pre_pid = -1;
        if (st == 1) {
            int slot = gfx_slot_for(gfx_pre_sig);
            if (slot >= 0 && !gcache[slot].sig[0]) {
                char tmp[PATH_MAX];
                gfx_tmp_path(tmp, sizeof tmp, slot);
                if (rename(gfx_pre_png, tmp) == 0 &&
                    png_size(tmp, &gcache[slot].iw, &gcache[slot].ih) == 0) {
                    snprintf(gcache[slot].sig, sizeof gcache[slot].sig, "%s", gfx_pre_sig);
                    snprintf(gcache[slot].png, sizeof gcache[slot].png, "%s", tmp);
                    gcache[slot].used = 1;    /* a guess, so it goes first */
                } else unlink(tmp);
            } else unlink(gfx_pre_png);
        } else unlink(gfx_pre_png);
        gfx_pre_sig[0] = 0;
        return;
    }
    if (!gfx_at.path[0] || gfx_at.page + 1 >= gfx_at.npg) return;
    char sig[PATH_MAX + 64];
    snprintf(sig, sizeof sig, "%s|%d|w%d", gfx_at.path, gfx_at.page + 1, gfx_at.width);
    if (gfx_find(sig) >= 0) return;                    /* already rendered */
    const char *dir = getenv("XDG_RUNTIME_DIR");
    if (!dir || !*dir) dir = "/tmp";
    snprintf(gfx_pre_png, sizeof gfx_pre_png, "%s/sds-%d-next.png", dir, (int)getpid());
    gfx_pre_pid = pdf_raster_start(gfx_at.path, gfx_at.page + 2, gfx_at.width,
                                   gfx_at.pt_w, gfx_pre_png);
    if (gfx_pre_pid < 0) gfx_pre_png[0] = 0;
    else snprintf(gfx_pre_sig, sizeof gfx_pre_sig, "%s", sig);
}
/* Is there rendering to look after? The main loop polls while there is. */
int gfx_busy(void) {
    if (gfx_pre_pid > 0) return 1;
    if (!gfx_at.path[0] || gfx_at.page + 1 >= gfx_at.npg) return 0;
    char sig[PATH_MAX + 64];
    snprintf(sig, sizeof sig, "%s|%d|w%d", gfx_at.path, gfx_at.page + 1, gfx_at.width);
    return gfx_find(sig) < 0;
}
/* Emit the frame's images. A page is transmitted once; after that, showing it
 * again — or scrolling inside it — is a placement, which is a few bytes. */
void gfx_flush(void) {
    int any = 0;
    for (int i = 0; i < MAX_PANES; i++) {
        if (!gfx[i].want) {
            if (gfx_live_slot[i] >= 0) {
                kitty_unplace(GFX_ID_BASE + gfx_live_slot[i], i + 1);
                gfx_live_slot[i] = -1;
                gfx_place_live[i][0] = 0;
                any = 1;
            }
            continue;
        }
        int slot = gfx[i].slot, id = GFX_ID_BASE + slot;
        if (!gcache[slot].live) {
            if (kitty_transmit(gcache[slot].png, id) != 0) { gfx[i].want = 0; continue; }
            gcache[slot].live = 1;
            any = 1;
        }
        if (gfx_live_slot[i] != slot) {        /* this pane moved to another page */
            if (gfx_live_slot[i] >= 0) kitty_unplace(GFX_ID_BASE + gfx_live_slot[i], i + 1);
            gfx_live_slot[i] = slot;
            gfx_place_live[i][0] = 0;
        }
        if (strcmp(gfx[i].place, gfx_place_live[i]) == 0) continue;
        kitty_unplace(id, i + 1);
        kitty_put(id, i + 1, gfx[i].y, gfx[i].x, gfx[i].cols, gfx[i].rows,
                  gfx[i].sx, gfx[i].sy, gfx[i].sw, gfx[i].sh);
        snprintf(gfx_place_live[i], sizeof gfx_place_live[i], "%s", gfx[i].place);
        any = 1;
    }
    if (any) fflush(stdout);
}
void gfx_clear_all(void) {
    for (int i = 0; i < GFX_CACHE; i++) gfx_drop(i);
    if (gfx_pre_pid > 0) {
        kill(gfx_pre_pid, SIGTERM);
        pdf_raster_done(gfx_pre_pid, gfx_pre_png, 1);
        gfx_pre_pid = -1;
    }
    if (gfx_pre_png[0]) unlink(gfx_pre_png);
    fflush(stdout);
}
