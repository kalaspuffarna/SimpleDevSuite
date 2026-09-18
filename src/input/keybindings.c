#include "core/sds.h"
#include "input/input.h"
#include "input/input_internal.h"

const KbDef kb_def[] = {
    { "quit",          ALT('q')          }, { "save",        CTRL('s')         },
    { "close_tab",     ALT('w')          }, { "help",        ALT('h')          },
    { "run",           ALT('r')          }, { "terminal",    ALT('t')          },
    { "find",          CTRL('f')         }, { "find_next",   KEY_F(3)          },
    { "replace",       CTRL('r')         }, { "goto",        CTRL('g')         },
    { "quick_open",    CTRL('p')         }, { "complete",    0 /* Ctrl+Space */},
    { "new_entry",     K_AINS            }, { "delete_entry", K_ADEL           },
    { "refresh",       KEY_F(5)          }, { "tree_up",     MK(3, D_UP)       },
    { "tree_down",     MK(3, D_DOWN)     }, { "tree_collapse", MK(3, D_LEFT)   },
    { "tree_expand",   MK(3, D_RIGHT)    }, { "tree_open",   ALT('\n')         },
    { "tree_open_pane", K_ASENTER        },
    { "tab_prev",      ALT(',')          }, { "tab_next",    ALT('.')          },
    { "wrap",          ALT('z')          }, { "sidebar",     ALT('b')          },
    { "move_line_up",  MK(6, D_UP)       }, { "move_line_down", MK(6, D_DOWN)  },
    { "pane_left",     MK(4, D_LEFT)     }, { "pane_right",  MK(4, D_RIGHT)    },
    { "pane_up",       MK(4, D_UP)       }, { "pane_down",   MK(4, D_DOWN)     },
    { "pane_close",    PKEY(0)           }, { "markdown",    ALT('m')          },
    { "line_scroll",   ALT('l')          },
};
int kb[KB_N];

/* Shifted digit rows, indexed by digit: entry 0 is Shift+0. Written as UTF-8
 * because several layouts put a non-ASCII character on one of the digits. */
static const struct { const char *name, *row; } kb_rows[] = {
    { "us",      ")!@#$%^&*("  },
    { "uk",      ")!\"\u00a3$%^&*(" },
    { "nordic",  "=!\"#\u00a4%&/()" },
    { "german",  "=!\"\u00a7$%&/()" },
    { "spanish", "=!\"\u00b7$%&/()" },
    { "italian", "=!\"\u00a3$%&/()" },
    { "french",  "0123456789"  },
};
#define NKBROW ((int)(sizeof kb_rows / sizeof *kb_rows))
static const char *kb_row = NULL;       /* active row, resolved on first use */
char kb_row_cfg[64] = "";        /* [keys] shifted_digits, if set */

static const char *kb_row_by_name(const char *n) {
    for (int i = 0; i < NKBROW; i++)
        if (!strcasecmp(kb_rows[i].name, n)) return kb_rows[i].row;
    return NULL;
}
/* xkb layout code → the row it types. */
static const char *kb_row_for_layout(const char *l) {
    static const struct { const char *l, *r; } m[] = {
        { "se", "nordic" }, { "fi", "nordic" }, { "no", "nordic" },
        { "nb", "nordic" }, { "dk", "nordic" }, { "da", "nordic" },
        { "is", "nordic" }, { "de", "german" }, { "at", "german" },
        { "ch", "german" }, { "gb", "uk"     }, { "es", "spanish" },
        { "it", "italian" }, { "fr", "french" }, { "be", "french" },
    };
    for (size_t i = 0; i < sizeof m / sizeof *m; i++)
        if (!strncasecmp(l, m[i].l, 2)) return kb_row_by_name(m[i].r);
    return NULL;
}
static int kb_probe(const char *cmd, const char *key, char *out, size_t cap) {
    FILE *f = popen(cmd, "r");
    if (!f) return 0;
    char line[256];
    int got = 0;
    size_t kl = strlen(key);
    while (!got && fgets(line, sizeof line, f)) {
        char *p = strstr(line, key);
        if (!p) continue;
        p += kl;
        while (*p == ' ' || *p == '\t' || *p == ':') p++;
        size_t n = strcspn(p, ",\r\n \t");        /* first of a comma list */
        if (n && n < cap) { snprintf(out, cap, "%.*s", (int)n, p); got = 1; }
    }
    pclose(f);
    return got;
}
/* Resolved lazily: nothing pays for this until an Alt+<symbol> key is hit. */
static const char *kb_digits(void) {
    if (kb_row) return kb_row;
    if (kb_row_cfg[0]) {
        const char *r = kb_row_by_name(kb_row_cfg);
        if (r) return kb_row = r;
        if (strcasecmp(kb_row_cfg, "auto") != 0) return kb_row = kb_row_cfg;
    }
    char lay[64] = "";
    const char *env = getenv("XKB_DEFAULT_LAYOUT");
    if (env && *env) snprintf(lay, sizeof lay, "%.*s", (int)strcspn(env, ","), env);
    if (!lay[0]) kb_probe("setxkbmap -query 2>/dev/null", "layout", lay, sizeof lay);
    if (!lay[0]) kb_probe("localectl status 2>/dev/null", "X11 Layout", lay, sizeof lay);
    const char *r = lay[0] ? kb_row_for_layout(lay) : NULL;
    return kb_row = (r ? r : kb_rows[0].row);
}
/* Is this UTF-8 character on the shifted digit row? Returns the digit or -1. */
int kb_digit_of(const char *ch, int len) {
    const char *row = kb_digits();
    for (int d = 0, i = 0; d < 10 && row[i]; d++) {
        int n = 1;
        while (row[i + n] && ((unsigned char)row[i + n] & 0xc0) == 0x80) n++;
        if (n == len && memcmp(row + i, ch, (size_t)len) == 0) return d;
        i += n;
    }
    return -1;
}

/* "ctrl+shift+left", "alt+q", "f5", "escape" … → an sds key code, or -1 */
int parse_key(const char *s) {
    int alt = 0, ctrl = 0, shift = 0;
    char buf[64];
    snprintf(buf, sizeof buf, "%s", s);
    for (char *p = buf; *p; p++) *p = (char)tolower((unsigned char)*p);

    char *k = buf;
    for (;;) {
        char *plus = strchr(k, '+');
        if (!plus) break;
        *plus = 0;
        if      (!strcmp(k, "ctrl") || !strcmp(k, "control")) ctrl = 1;
        else if (!strcmp(k, "alt")  || !strcmp(k, "meta"))    alt = 1;
        else if (!strcmp(k, "shift"))                         shift = 1;
        else return -1;
        k = plus + 1;
    }
    if (!*k) return -1;

    /* arrows / home / end take the modifier-encoding path */
    int dir = -1;
    if      (!strcmp(k, "up"))    dir = D_UP;
    else if (!strcmp(k, "down"))  dir = D_DOWN;
    else if (!strcmp(k, "left"))  dir = D_LEFT;
    else if (!strcmp(k, "right")) dir = D_RIGHT;
    else if (!strcmp(k, "home"))  dir = D_HOME;
    else if (!strcmp(k, "end"))   dir = D_END;
    if (dir >= 0) {
        int mod = 1 + shift + 2 * alt + 4 * ctrl;      /* xterm's scheme */
        if (mod == 1) {
            switch (dir) {
                case D_UP:    return KEY_UP;
                case D_DOWN:  return KEY_DOWN;
                case D_LEFT:  return KEY_LEFT;
                case D_RIGHT: return KEY_RIGHT;
                case D_HOME:  return KEY_HOME;
                default:      return KEY_END;
            }
        }
        return MK(mod, dir);
    }
    if (k[0] == 'f' && isdigit((unsigned char)k[1])) {
        int n = atoi(k + 1);
        if (n >= 1 && n <= 12) return KEY_F(n);
        return -1;
    }
    if (!strcmp(k, "insert")) return alt ? K_AINS : KEY_IC;
    if (!strcmp(k, "delete")) return alt ? K_ADEL : KEY_DC;
    if (!strcmp(k, "enter") || !strcmp(k, "return"))
        return alt ? (shift ? K_ASENTER : ALT('\n')) : '\r';
    if (!strcmp(k, "escape") || !strcmp(k, "esc")) return alt ? ALT(27) : 27;
    if (!strcmp(k, "space"))  return ctrl ? 0 : (alt ? ALT(' ') : ' ');
    if (!strcmp(k, "tab"))    return '\t';
    if (!strcmp(k, "pageup"))   return KEY_PPAGE;
    if (!strcmp(k, "pagedown")) return KEY_NPAGE;
    if (k[1]) return -1;                          /* multi-char, unrecognised */
    /* "alt+shift+3" means the 3 key, whatever character that types on this
     * keyboard; the reader resolves it through the layout. */
    if (shift && alt && !ctrl && isdigit((unsigned char)k[0]))
        return PKEY(k[0] - '0');
    if (ctrl) return CTRL(k[0]);
    if (alt)  return ALT(k[0]);
    return (unsigned char)k[0];
}
