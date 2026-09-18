#include "core/sds.h"
#include "config/config.h"
#include "input/input.h"
#include "pdf/pdf.h"
#include "markdown/markdown.h"
#include "ui/ui.h"
#include "tree/tree.h"
#include "lsp/lsp.h"

/* ── config ───────────────────────────────────────────────────────── */
static char cfg_dir[PATH_MAX];
char cfg_warn[256] = "";     /* shown once in the status bar at startup */

static void cfg_dir_init(void) {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    if (xdg && *xdg) snprintf(cfg_dir, sizeof cfg_dir, "%s/sds", xdg);
    else if (home && *home) snprintf(cfg_dir, sizeof cfg_dir, "%s/.config/sds", home);
    else cfg_dir[0] = 0;
}
/* strip a trailing comment and surrounding whitespace/quotes, in place */
static char *cfg_clean(char *s) {
    int inq = 0;
    for (char *p = s; *p; p++) {
        if (*p == '"') inq = !inq;
        else if ((*p == '#' || *p == ';') && !inq) { *p = 0; break; }
    }
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) e--;
    *e = 0;
    if (e > s + 1 && *s == '"' && e[-1] == '"') { s++; e[-1] = 0; }
    return s;
}
static int parse_hex(const char *s, int *out) {
    if (*s == '#') s++;
    if (strlen(s) != 6) return 0;
    char *end;
    long v = strtol(s, &end, 16);
    if (*end) return 0;
    *out = (int)v;
    return 1;
}
/* assign one "key = value" into a theme; returns 1 if the key was a color */
static int theme_set(Theme *t, const char *k, const char *v) {
    static const char *names[] = { "accent", "bg", "fg", "muted", "bg_alt",
                                   "error", "kw", "type", "str", "com",
                                   "num", "pre" };
    Col *slots[] = { &t->accent, &t->bg, &t->fg, &t->muted, &t->bg_alt,
                     &t->error, &t->kw, &t->type, &t->str, &t->com,
                     &t->num, &t->pre };
    for (size_t i = 0; i < sizeof names / sizeof *names; i++)
        if (!strcmp(k, names[i])) {
            int rgb;
            if (!parse_hex(v, &rgb)) return 1;      /* claimed, but unusable */
            slots[i]->rgb = rgb;
            /* a file-supplied color has no declared 8-color fallback; pick the
             * nearest basic color so 8-color terminals still differentiate */
            static const int b8[8] = { 0x000000, 0xcc0000, 0x4e9a06, 0xc4a000,
                                       0x3465a4, 0x75507b, 0x06989a, 0xd3d7cf };
            int best = 0, bd = 1 << 30;
            for (int c = 0; c < 8; c++) {
                int dr = (b8[c] >> 16 & 0xff) - (rgb >> 16 & 0xff);
                int dg = (b8[c] >> 8  & 0xff) - (rgb >> 8  & 0xff);
                int db = (b8[c]       & 0xff) - (rgb       & 0xff);
                int d = dr * dr + dg * dg + db * db;
                if (d < bd) { bd = d; best = c; }
            }
            slots[i]->basic = (short)best;
            return 1;
        }
    return 0;
}
/* Scan a themes file for [want] and load it into `out`. Returns 1 if found. */
static int themes_file_load(const char *path, const char *want, Theme *out) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[512];
    int in = 0, found = 0;
    while (fgets(line, sizeof line, f)) {
        char *s = cfg_clean(line);
        if (!*s) continue;
        if (*s == '[') {
            char *e = strchr(s, ']');
            if (!e) continue;
            *e = 0;
            in = !strcmp(s + 1, want);
            if (in) { found = 1; snprintf(out->name, sizeof out->name, "%s", want); }
            continue;
        }
        if (!in) continue;
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        theme_set(out, cfg_clean(s), cfg_clean(eq + 1));
    }
    fclose(f);
    return found;
}

static const char *DEFAULT_CONFIG =
"# SimpleDevSuite configuration.\n"
"#\n"
"# Every key below is optional — anything you leave out (or delete) falls back\n"
"# to the built-in default shown here. Unknown keys are ignored rather than\n"
"# treated as errors. Nothing hot-reloads; restart sds after editing.\n"
"\n"
"# Color theme. \"tux\" is black/white/amber after the penguin. \"classic\" is\n"
"# sds's original eight-color look. Also built in: monokai, dracula, nord,\n"
"# gruvbox. Any [name] table in the `themes` file next to this one also\n"
"# works, and overrides a built-in of the same name.\n"
"theme = \"tux\"\n"
"\n"
"# Show the theme's exact colors instead of snapping them to the terminal's\n"
"# fixed 256-color palette. Needs a terminal that allows redefining palette\n"
"# entries (terminfo `ccc`); sds detects that and quietly falls back if not.\n"
"# It borrows a few slots from the top of the palette and restores them on\n"
"# exit. Set to off if colors inside terminal tabs look wrong.\n"
"true_color = on\n"
"\n"
"# Click tabs, the tree and the text; drag to select; wheel to scroll. While\n"
"# this is on the terminal hands pointer events to sds instead of acting on\n"
"# them itself — hold Shift to get the terminal's own select-and-copy back,\n"
"# or set this to off if you would rather never give it up.\n"
"mouse = on\n"
"\n"
"[editor]\n"
"tab_width = 4        # render width of a tab character (1-16)\n"
"soft_wrap = false    # start with word wrap on (toggle at runtime with Alt+Z)\n"
"# With wrap off, scroll only the line the cursor is on sideways and keep the\n"
"# others at their start; off scrolls the whole view together. Alt+L toggles.\n"
"line_scroll = on\n"
"# Tree-sitter parses a file whole: the first parse of a large one takes a\n"
"# moment and every edit after it costs time proportional to the file. Past\n"
"# this size the built-in lexer takes over, which works line by line. 0 means\n"
"# no limit. (Only matters in a build with tree-sitter support.)\n"
"treesitter_max_kb = 512\n"
"\n"
"[pdf]\n"
"# Show the real rendered page instead of extracted text, when the terminal\n"
"# speaks the kitty graphics protocol (kitty, ghostty, WezTerm, iTerm2) and\n"
"# one of mutool, pdftoppm or gs is installed. Falls back to text with a\n"
"# message when either is missing. `v` toggles the two at runtime.\n"
"render = on\n"
"# How large a page starts out, as a percentage of the pane's width: 100 fills\n"
"# the pane edge to edge and scrolls with Up/Down, which stays readable in a\n"
"# half-width pane. +/- change it while reading, 0 comes back here. (25-800)\n"
"zoom = 100\n"
"\n"
"[markdown]\n"
"# A .md file opens as text you can edit; the markdown key (Alt+M) swaps the\n"
"# pane between that source and a rendered view of it — headings, lists,\n"
"# tables, quotes and code blocks laid out for the width of the pane. Set\n"
"# this to on to have .md files open rendered instead.\n"
"preview = off\n"
"# Cap the rendered text at this many columns, so a wide pane doesn't produce\n"
"# lines too long to read comfortably. 0 uses the whole pane. (20-200)\n"
"width = 0\n"
"\n"
"[lsp]\n"
"# Compile errors and warnings as you type, shown in a gap under the line they\n"
"# belong to. C and C++ go to clangd (from the clang package), which needs to\n"
"# know how the project is built to get includes and flags right: point it at\n"
"# a compile_commands.json (CMake: -DCMAKE_EXPORT_COMPILE_COMMANDS=ON; make:\n"
"# bear -- make) in the project or its build/ directory, or a compile_flags.txt.\n"
"enabled = on\n"
"clangd = \"clangd\"   # command to run; may carry arguments\n"
"\n"
"[tree]\n"
"width = 30           # sidebar width in columns\n"
"# Below this terminal width the sidebar hides itself so the editor stays\n"
"# usable. Collapsing past the top level (Alt+Left) also hides it; Alt+Right\n"
"# or Alt+B brings it back. Set to 0 to never auto-hide.\n"
"auto_hide_below = 80\n"
"\n"
"[keys]\n"
"# Syntax: lowercase, \"+\"-joined, e.g. \"ctrl+s\", \"alt+shift+up\", \"f5\".\n"
"# Only these app-level actions are remappable; in-editor chords are not.\n"
"#\n"
"# The tree keys default to Alt+<something>. If your window manager grabs Alt\n"
"# (common on i3/sway/Hyprland), remap them to combos it doesn't intercept.\n"
"quit          = \"alt+q\"\n"
"save          = \"ctrl+s\"\n"
"close_tab     = \"alt+w\"\n"
"help          = \"alt+h\"\n"
"run           = \"alt+r\"\n"
"terminal      = \"alt+t\"\n"
"find          = \"ctrl+f\"\n"
"find_next     = \"f3\"\n"
"replace       = \"ctrl+r\"\n"
"goto          = \"ctrl+g\"\n"
"quick_open    = \"ctrl+p\"\n"
"complete      = \"ctrl+space\"\n"
"new_entry     = \"alt+insert\"\n"
"delete_entry  = \"alt+delete\"\n"
"refresh       = \"f5\"\n"
"tree_up       = \"alt+up\"\n"
"tree_down     = \"alt+down\"\n"
"tree_collapse = \"alt+left\"\n"
"tree_expand   = \"alt+right\"\n"
"tree_open     = \"alt+enter\"\n"
"# Opens the selected file in a pane of its own, next to the one you are in.\n"
"# Needs a terminal that can report Shift with Enter (sds asks for xterm's\n"
"# modifyOtherKeys at startup); where it can't, this arrives as plain\n"
"# Alt+Enter and opens in the current pane instead.\n"
"tree_open_pane = \"alt+shift+enter\"\n"
"tab_prev      = \"alt+,\"\n"
"tab_next      = \"alt+.\"\n"
"wrap          = \"alt+z\"\n"
"sidebar       = \"alt+b\"\n"
"markdown      = \"alt+m\"   # rendered .md <-> its source\n"
"line_scroll   = \"alt+l\"   # sideways scroll: current line / whole view\n"
"\n"
"# Split view. Alt+Shift+1..9 puts that tab in its own pane (up to four, in a\n"
"# 2x2 grid); pressing it again on the pane you are in folds that pane away.\n"
"# The arrows move focus between panes geometrically.\n"
"pane_left     = \"alt+shift+left\"\n"
"pane_right    = \"alt+shift+right\"\n"
"pane_up       = \"alt+shift+up\"\n"
"pane_down     = \"alt+shift+down\"\n"
"pane_close    = \"alt+shift+0\"\n"
"\n"
"# Which characters your keyboard types for Shift+0..Shift+9. A terminal\n"
"# never reports \"shift and the 2 key\", only the character it produces, and\n"
"# that differs per layout (Shift+2 is @ on a US board, \" on a Swedish one).\n"
"# \"auto\" reads the active X keyboard layout. You can also name a layout —\n"
"# us, uk, nordic, german, spanish, italian, french — or spell the row out,\n"
"# ten characters starting with Shift+0, e.g. \"=!\\\"#\u00a4%&/()\".\n"
"shifted_digits = \"auto\"\n"
"\n"
"# Moving lines lived on Alt+Shift+Up/Down before the panes took those over.\n"
"move_line_up   = \"ctrl+shift+up\"\n"
"move_line_down = \"ctrl+shift+down\"\n";

static const char *DEFAULT_THEMES =
"# SimpleDevSuite themes.\n"
"#\n"
"# Select one from your `config` with e.g. theme = \"monokai\". A [name] table\n"
"# here overrides a built-in theme of the same name, so you can retune \"tux\"\n"
"# without renaming it. Add your own by copying a block and changing the name.\n"
"#\n"
"# Twelve roles, all \"#rrggbb\". Missing ones inherit the built-in default.\n"
"#   accent  active tab, tree selection, line numbers\n"
"#   bg      primary background (and the text color on accent surfaces)\n"
"#   fg      primary text\n"
"#   muted   tree rules, dim text, the inactive UI\n"
"#   bg_alt  tab bar background\n"
"#   error   error dialogs and messages\n"
"#   kw type str com num pre    syntax: keywords, types, strings,\n"
"#                              comments, numbers, preprocessor\n"
"\n"
"# Tux chrome with VS Code dark syntax: the UI is the penguin (black body,\n"
"# white belly, amber beak), while the code itself uses VS Code's default\n"
"# dark colors, which separate better by hue than an all-amber palette.\n"
"[tux]\n"
"accent = \"#f5a623\"  # the beak — tabs, tree selection, line numbers\n"
"bg     = \"#000000\"  # the body\n"
"fg     = \"#ffffff\"  # the belly\n"
"muted  = \"#5f5f5f\"\n"
"bg_alt = \"#141414\"\n"
"error  = \"#e0503a\"\n"
"kw     = \"#569cd6\"  # VS Code blue\n"
"type   = \"#4ec9b0\"  # teal\n"
"str    = \"#ce9178\"  # salmon\n"
"com    = \"#6a9955\"  # green\n"
"num    = \"#b5cea8\"  # pale green\n"
"pre    = \"#c586c0\"  # purple\n"
"\n"
"# sds's original look, on the eight basic terminal colors.\n"
"[classic]\n"
"accent = \"#f5a623\"\n"
"bg     = \"#000000\"\n"
"fg     = \"#ffffff\"\n"
"muted  = \"#3465a4\"\n"
"bg_alt = \"#1a1a1a\"\n"
"error  = \"#d23c3d\"\n"
"kw     = \"#ad7fa8\"\n"
"type   = \"#34e2e2\"\n"
"str    = \"#8ae234\"\n"
"com    = \"#3465a4\"\n"
"num    = \"#ef2929\"\n"
"pre    = \"#34e2e2\"\n"
"\n"
"[monokai]\n"
"accent = \"#fd971f\"\n"
"bg     = \"#272822\"\n"
"fg     = \"#f8f8f2\"\n"
"muted  = \"#75715e\"\n"
"bg_alt = \"#3e3d32\"\n"
"error  = \"#f92672\"\n"
"kw     = \"#f92672\"\n"
"type   = \"#66d9ef\"\n"
"str    = \"#e6db74\"\n"
"com    = \"#75715e\"\n"
"num    = \"#ae81ff\"\n"
"pre    = \"#a6e22e\"\n"
"\n"
"[dracula]\n"
"accent = \"#bd93f9\"\n"
"bg     = \"#282a36\"\n"
"fg     = \"#f8f8f2\"\n"
"muted  = \"#6272a4\"\n"
"bg_alt = \"#44475a\"\n"
"error  = \"#ff5555\"\n"
"kw     = \"#ff79c6\"\n"
"type   = \"#8be9fd\"\n"
"str    = \"#f1fa8c\"\n"
"com    = \"#6272a4\"\n"
"num    = \"#bd93f9\"\n"
"pre    = \"#50fa7b\"\n"
"\n"
"[nord]\n"
"accent = \"#88c0d0\"\n"
"bg     = \"#2e3440\"\n"
"fg     = \"#d8dee9\"\n"
"muted  = \"#4c566a\"\n"
"bg_alt = \"#3b4252\"\n"
"error  = \"#bf616a\"\n"
"kw     = \"#81a1c1\"\n"
"type   = \"#8fbcbb\"\n"
"str    = \"#a3be8c\"\n"
"com    = \"#616e88\"\n"
"num    = \"#b48ead\"\n"
"pre    = \"#5e81ac\"\n"
"\n"
"[gruvbox]\n"
"accent = \"#fe8019\"\n"
"bg     = \"#282828\"\n"
"fg     = \"#ebdbb2\"\n"
"muted  = \"#928374\"\n"
"bg_alt = \"#3c3836\"\n"
"error  = \"#fb4934\"\n"
"kw     = \"#fb4934\"\n"
"type   = \"#8ec07c\"\n"
"str    = \"#b8bb26\"\n"
"com    = \"#928374\"\n"
"num    = \"#d3869b\"\n"
"pre    = \"#fabd2f\"\n";

static int write_if_absent(const char *path, const char *body) {
    if (access(path, F_OK) == 0) return 0;
    FILE *f = fopen(path, "w");
    if (!f) return 0;
    fputs(body, f);
    fclose(f);
    return 1;
}
/* Create ~/.config/sds/{config,themes,syntax/} the first time sds runs.
 * Best-effort: a read-only home just means the built-in defaults apply. */
static int cfg_write_defaults(void) {
    if (!cfg_dir[0]) return 0;
    char p[PATH_MAX];
    const char *home = getenv("HOME");
    if (home && *home) {                        /* ensure ~/.config exists */
        snprintf(p, sizeof p, "%s/.config", home);
        mkdir(p, 0755);
    }
    if (mkdir(cfg_dir, 0755) != 0 && errno != EEXIST) return 0;
    snprintf(p, sizeof p, "%s/syntax", cfg_dir);
    mkdir(p, 0755);
    int wrote = 0;
    snprintf(p, sizeof p, "%s/config", cfg_dir);
    wrote |= write_if_absent(p, DEFAULT_CONFIG);
    snprintf(p, sizeof p, "%s/themes", cfg_dir);
    wrote |= write_if_absent(p, DEFAULT_THEMES);
    return wrote;
}
/* Read the config, resolve the theme, and fill kb[]. Never fails hard: a
 * malformed file leaves the built-in defaults in place and sets cfg_warn. */
void cfg_load(void) {
    char want[32] = "tux";
    for (int i = 0; i < KB_N; i++) kb[i] = kb_def[i].dflt;

    cfg_dir_init();
    int fresh = cfg_write_defaults();

    char path[PATH_MAX];
    if (cfg_dir[0]) {
        snprintf(path, sizeof path, "%s/config", cfg_dir);
        FILE *f = fopen(path, "r");
        if (f) {
            char line[512];
            char sect[32] = "";
            int bad = 0;
            while (fgets(line, sizeof line, f)) {
                char *s = cfg_clean(line);
                if (!*s) continue;
                if (*s == '[') {
                    char *e = strchr(s, ']');
                    if (!e) { bad++; continue; }
                    *e = 0;
                    snprintf(sect, sizeof sect, "%s", s + 1);
                    continue;
                }
                char *eq = strchr(s, '=');
                if (!eq) { bad++; continue; }
                *eq = 0;
                char *k = cfg_clean(s), *v = cfg_clean(eq + 1);

                if (!sect[0] && !strcmp(k, "theme")) {
                    snprintf(want, sizeof want, "%s", v);
                } else if (!sect[0] && !strcmp(k, "true_color")) {
                    tc_want = !(!strcmp(v, "false") || !strcmp(v, "off") ||
                                !strcmp(v, "0"));
                } else if (!sect[0] && !strcmp(k, "mouse")) {
                    mouse_cfg = !(!strcmp(v, "false") || !strcmp(v, "off") ||
                                  !strcmp(v, "0"));
                } else if (!strcmp(sect, "editor")) {
                    if (!strcmp(k, "tab_width")) {
                        int n = atoi(v);
                        if (n >= 1 && n <= TABSTOP_MAX) tabstop = n;
                    } else if (!strcmp(k, "soft_wrap")) {
                        wrap = !strcmp(v, "true") || !strcmp(v, "1");
                    } else if (!strcmp(k, "treesitter_max_kb")) {
                        int n = atoi(v);
                        if (n >= 0) cfg_ts_max_kb = n;
                    } else if (!strcmp(k, "line_scroll")) {
                        line_scroll = !(!strcmp(v, "false") || !strcmp(v, "off") ||
                                        !strcmp(v, "0"));
                    }
                } else if (!strcmp(sect, "pdf")) {
                    if (!strcmp(k, "render"))
                        cfg_pdf_render = !(!strcmp(v, "false") ||
                                           !strcmp(v, "off") || !strcmp(v, "0"));
                    else if (!strcmp(k, "zoom")) {
                        /* percent of the pane width a page starts at */
                        double z = atof(v) / 100.0;
                        if (z >= PDF_ZOOM_MIN && z <= PDF_ZOOM_MAX) cfg_pdf_zoom = z;
                    }
                } else if (!strcmp(sect, "markdown")) {
                    if (!strcmp(k, "preview"))
                        cfg_md_preview = !strcmp(v, "true") || !strcmp(v, "on") ||
                                         !strcmp(v, "1");
                    else if (!strcmp(k, "width")) {
                        int n = atoi(v);
                        if (n == 0 || (n >= 20 && n <= 200)) cfg_md_width = n;
                    }
                } else if (!strcmp(sect, "lsp")) {
                    if (!strcmp(k, "enabled"))
                        cfg_lsp_enabled = !(!strcmp(v, "false") || !strcmp(v, "off") ||
                                            !strcmp(v, "0"));
                    else if (!strcmp(k, "clangd"))
                        snprintf(cfg_clangd, sizeof cfg_clangd, "%s", v);
                } else if (!strcmp(sect, "tree")) {
                    if (!strcmp(k, "width")) {
                        int n = atoi(v);
                        if (n >= 10 && n <= 100) tree_w = n;
                    } else if (!strcmp(k, "auto_hide_below")) {
                        tree_autohide = atoi(v);
                    }
                } else if (!strcmp(sect, "keys")) {
                    if (!strcmp(k, "shifted_digits")) {
                        snprintf(kb_row_cfg, sizeof kb_row_cfg, "%s", v);
                        continue;
                    }
                    for (int i = 0; i < KB_N; i++)
                        if (!strcmp(k, kb_def[i].name)) {
                            int c = parse_key(v);
                            if (c >= 0) kb[i] = c;
                            else bad++;
                            break;
                        }
                }
            }
            fclose(f);
            if (bad)
                snprintf(cfg_warn, sizeof cfg_warn,
                         "config: %d unusable line(s), using defaults there", bad);
        }
    }

    /* built-in theme first, then let a themes file of the same name override */
    theme = theme_tux;
    for (int i = 0; i < ntheme_presets; i++)
        if (!strcmp(theme_presets[i].name, want)) { theme = theme_presets[i]; break; }
    int known = !strcmp(theme.name, want);
    if (cfg_dir[0]) {
        snprintf(path, sizeof path, "%s/themes", cfg_dir);
        if (themes_file_load(path, want, &theme)) known = 1;
    }
    if (!known && !cfg_warn[0])
        snprintf(cfg_warn, sizeof cfg_warn, "unknown theme \"%s\" — using tux", want);
    if (fresh && !cfg_warn[0])
        snprintf(cfg_warn, sizeof cfg_warn, "wrote default config to %s", cfg_dir);
}
