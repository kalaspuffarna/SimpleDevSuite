#include "core/sds.h"
#include "ui/ui.h"
#include "ui/ui_internal.h"

/* Tux chrome, VS Code syntax.
 *
 * The UI is the penguin: a black body, a white belly, and the amber beak
 * (#f5a623) for the accent — tabs, tree selection, line numbers. Carrying
 * that amber into the syntax colors as well turned the code itself too
 * yellow, so those follow VS Code's default dark theme instead, which is
 * both familiar and well separated by hue. */
const Theme theme_tux = {
    "tux",
    { 0xf5a623, COLOR_YELLOW }, { 0x000000, COLOR_BLACK },   /* accent, bg    */
    { 0xffffff, COLOR_WHITE  }, { 0x5f5f5f, COLOR_BLUE  },   /* fg,     muted */
    { 0x141414, COLOR_BLACK  }, { 0xe0503a, COLOR_RED   },   /* bg_alt, error */
    { 0x569cd6, COLOR_BLUE   }, { 0x4ec9b0, COLOR_CYAN  },   /* kw,     type  */
    { 0xce9178, COLOR_RED    }, { 0x6a9955, COLOR_GREEN },   /* str,    com   */
    { 0xb5cea8, COLOR_YELLOW }, { 0xc586c0, COLOR_MAGENTA }, /* num,    pre   */
};

/* Presets carried over from the Python-era themes.toml. Their syntax colors
 * are derived from each palette rather than invented. */
const Theme theme_presets[] = {
    /* sds's pre-theme look: the eight basic terminal colors, kept so the
     * original appearance is still one config line away. */
    { "classic",
      { 0xf5a623, COLOR_YELLOW  }, { 0x000000, COLOR_BLACK },
      { 0xffffff, COLOR_WHITE   }, { 0x3465a4, COLOR_BLUE  },
      { 0x1a1a1a, COLOR_BLACK   }, { 0xd23c3d, COLOR_RED   },
      { 0xad7fa8, COLOR_MAGENTA }, { 0x34e2e2, COLOR_CYAN  },
      { 0x8ae234, COLOR_GREEN   }, { 0x3465a4, COLOR_BLUE  },
      { 0xef2929, COLOR_RED     }, { 0x34e2e2, COLOR_CYAN  } },
    { "monokai",
      { 0xfd971f, COLOR_YELLOW  }, { 0x272822, COLOR_BLACK },
      { 0xf8f8f2, COLOR_WHITE   }, { 0x75715e, COLOR_BLUE  },
      { 0x3e3d32, COLOR_BLACK   }, { 0xf92672, COLOR_RED   },
      { 0xf92672, COLOR_MAGENTA }, { 0x66d9ef, COLOR_CYAN  },
      { 0xe6db74, COLOR_GREEN   }, { 0x75715e, COLOR_BLUE  },
      { 0xae81ff, COLOR_RED     }, { 0xa6e22e, COLOR_CYAN  } },
    { "dracula",
      { 0xbd93f9, COLOR_YELLOW  }, { 0x282a36, COLOR_BLACK },
      { 0xf8f8f2, COLOR_WHITE   }, { 0x6272a4, COLOR_BLUE  },
      { 0x44475a, COLOR_BLACK   }, { 0xff5555, COLOR_RED   },
      { 0xff79c6, COLOR_MAGENTA }, { 0x8be9fd, COLOR_CYAN  },
      { 0xf1fa8c, COLOR_GREEN   }, { 0x6272a4, COLOR_BLUE  },
      { 0xbd93f9, COLOR_RED     }, { 0x50fa7b, COLOR_CYAN  } },
    { "nord",
      { 0x88c0d0, COLOR_YELLOW  }, { 0x2e3440, COLOR_BLACK },
      { 0xd8dee9, COLOR_WHITE   }, { 0x4c566a, COLOR_BLUE  },
      { 0x3b4252, COLOR_BLACK   }, { 0xbf616a, COLOR_RED   },
      { 0x81a1c1, COLOR_MAGENTA }, { 0x8fbcbb, COLOR_CYAN  },
      { 0xa3be8c, COLOR_GREEN   }, { 0x616e88, COLOR_BLUE  },
      { 0xb48ead, COLOR_RED     }, { 0x5e81ac, COLOR_CYAN  } },
    { "gruvbox",
      { 0xfe8019, COLOR_YELLOW  }, { 0x282828, COLOR_BLACK },
      { 0xebdbb2, COLOR_WHITE   }, { 0x928374, COLOR_BLUE  },
      { 0x3c3836, COLOR_BLACK   }, { 0xfb4934, COLOR_RED   },
      { 0xfb4934, COLOR_MAGENTA }, { 0x8ec07c, COLOR_CYAN  },
      { 0xb8bb26, COLOR_GREEN   }, { 0x928374, COLOR_BLUE  },
      { 0xd3869b, COLOR_RED     }, { 0xfabd2f, COLOR_CYAN  } },
};
const int ntheme_presets = (int)(sizeof theme_presets / sizeof *theme_presets);

Theme theme;                 /* the active theme */

/* nearest xterm-256 index for an rgb triple: try the 6×6×6 cube and the
 * 24-step grey ramp, keep whichever is closer */
int rgb_to_256(int rgb) {
    int r = rgb >> 16 & 0xff, g = rgb >> 8 & 0xff, b = rgb & 0xff;
    static const int lv[6] = { 0, 95, 135, 175, 215, 255 };
    int ci[3], comp[3] = { r, g, b };
    for (int k = 0; k < 3; k++) {
        int best = 0, bd = 1 << 30;
        for (int i = 0; i < 6; i++) {
            int d = abs(lv[i] - comp[k]);
            if (d < bd) { bd = d; best = i; }
        }
        ci[k] = best;
    }
    int cube = 16 + 36 * ci[0] + 6 * ci[1] + ci[2];
    int cd = 0;
    for (int k = 0; k < 3; k++) { int d = lv[ci[k]] - comp[k]; cd += d * d; }

    int grey = (r * 299 + g * 587 + b * 114) / 1000;
    int gi = (grey - 8) / 10;
    if (gi < 0) gi = 0;
    if (gi > 23) gi = 23;
    int gv = 8 + gi * 10, gd = 0;
    for (int k = 0; k < 3; k++) { int d = gv - comp[k]; gd += d * d; }

    return gd < cd ? 232 + gi : cube;
}
/* Exact colors.
 *
 * A 256-color terminal can only show the fixed xterm palette, so theme colors
 * normally snap to the nearest entry (#f5a623 lands on #ffaf00). Most modern
 * terminals can however be told what a palette slot *means*, which terminfo
 * reports as `ccc` and ncurses as can_change_color(). When that is available
 * sds redefines a handful of slots, taken from the top of the palette, to the
 * theme's exact RGB — and puts them back on exit so the shell's colors are
 * not left altered.
 *
 * Slots are shared with whatever runs in a terminal tab, which is why they
 * come from the top (the light end of the greyscale ramp, rarely referenced
 * by name) and why `true_color = off` exists in the config.              */
#define TC_SLOTS 16
int   tc_want = 1;               /* config: 1 = use exact colors */
static int   tc_on   = 0;               /* resolved at startup */
static int   tc_rgb[TC_SLOTS];          /* rgb held by each allocated slot */
static short tc_idx[TC_SLOTS];
static int   tc_n = 0;

/* The canonical xterm-256 value of a palette index: 16-231 are the 6×6×6
 * cube, 232-255 the greyscale ramp. Restoring has to be computed like this
 * rather than read back with color_content(), which only knows the first
 * few entries and reports nonsense above them — using it would leave the
 * palette worse than it started. */
static int xterm256_rgb(int i) {
    static const int lv[6] = { 0, 95, 135, 175, 215, 255 };
    if (i >= 232 && i <= 255) { int v = 8 + (i - 232) * 10; return (v << 16) | (v << 8) | v; }
    if (i >= 16 && i <= 231) {
        i -= 16;
        return (lv[i / 36] << 16) | (lv[(i / 6) % 6] << 8) | lv[i % 6];
    }
    return 0;
}
/* 0-255 to curses' 0-1000, rounded so the value survives the round trip:
 * terminfo's initc converts back with a truncating (v*255)/1000, so picking
 * the smallest scaled value that floors to v is what lands exactly on v.
 * Truncating here instead loses one unit per channel. */
static short tc_scale(int v) {
    int x = (v * 1000 + 254) / 255;         /* ceil */
    return (short)(x > 1000 ? 1000 : x);
}
static void tc_set(short idx, int rgb) {
    init_color(idx, tc_scale(rgb >> 16 & 0xff),
                    tc_scale(rgb >> 8  & 0xff),
                    tc_scale(rgb       & 0xff));
}
static int tc_alloc(int rgb) {
    for (int i = 0; i < tc_n; i++) if (tc_rgb[i] == rgb) return tc_idx[i];
    if (tc_n >= TC_SLOTS) return rgb_to_256(rgb);      /* out of slots */
    short idx = (short)(COLORS - 1 - tc_n);
    if (idx < 16 || idx > 255) return rgb_to_256(rgb);
    tc_set(idx, rgb);
    tc_rgb[tc_n] = rgb;
    tc_idx[tc_n] = idx;
    tc_n++;
    return idx;
}
/* hand the palette back the way a 256-color terminal defines it */
void tc_restore(void) {
    for (int i = 0; i < tc_n; i++) tc_set(tc_idx[i], xterm256_rgb(tc_idx[i]));
    tc_n = 0;
}
/* map a theme color onto whatever this terminal can actually show */
static int col_of(Col c) {
    if (tc_on)         return tc_alloc(c.rgb);
    if (COLORS >= 256) return rgb_to_256(c.rgb);
    return c.basic;
}
void apply_theme(void) {
    if (!has_colors()) return;
    tc_restore();                       /* re-theming reuses the same slots */
    tc_on = tc_want && can_change_color() && COLORS >= 256;
    int bg = col_of(theme.bg), fg = col_of(theme.fg);
    /* -1 keeps the terminal's own background, which is what sds did before and
     * what makes transparent terminals look right */
    int dfl = (theme.bg.rgb == 0x000000) ? -1 : bg;
    init_pair(CP_TAB_ACT, bg,                  col_of(theme.accent));
    init_pair(CP_TAB,     fg,                  col_of(theme.bg_alt));
    init_pair(CP_SEL,     bg,                  col_of(theme.accent));
    init_pair(CP_DIR,     col_of(theme.type),  dfl);
    init_pair(CP_STATUS,  bg,                  fg);
    init_pair(CP_LINENO,  col_of(theme.accent), dfl);
    init_pair(CP_MUTED,   col_of(theme.muted), dfl);
    init_pair(CP_KW,      col_of(theme.kw),    dfl);
    init_pair(CP_TYPE,    col_of(theme.type),  dfl);
    init_pair(CP_STR,     col_of(theme.str),   dfl);
    init_pair(CP_COM,     col_of(theme.com),   dfl);
    init_pair(CP_NUM,     col_of(theme.num),   dfl);
    init_pair(CP_PRE,     col_of(theme.pre),   dfl);
    init_pair(CP_FIND,    bg,                  col_of(theme.str));
    init_pair(CP_ERR,     fg,                  col_of(theme.error));
    init_pair(CP_HEAD,    col_of(theme.accent), dfl);   /* markdown headings */
    init_pair(CP_DIAG_ERR,  col_of(theme.error),  dfl); /* diagnostics under a line */
    init_pair(CP_DIAG_WARN, col_of(theme.accent), dfl);
    init_pair(CP_DIAG_INFO, col_of(theme.muted),  dfl);
}
