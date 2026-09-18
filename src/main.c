/* main.c — SimpleDevSuite v2
 *
 * A small terminal dev environment inspired by VS Code:
 *
 *   - file tree (left, collapsible), tab bar (top), editor with line
 *     numbers, status bar
 *   - syntax highlighting: C, C++, Python, Bash, Rust, SQL, JS/TS, Go,
 *     Java, Lua, Ruby, PHP, JSON, TOML/YAML/INI, Makefile, Markdown —
 *     optionally via tree-sitter when a grammar is installed (see below)
 *   - embedded pty terminal tabs (Alt+T) with scrollback and colors
 *   - git status markers in the tree and the branch in the status bar
 *   - config file and themes under ~/.config/sds/
 *   - undo/redo, selections, clipboard (with OSC 52 system-clipboard copy)
 *   - incremental find, replace, go-to-line
 *   - fuzzy quick-open (Ctrl+P), word-based autocomplete (Ctrl+Space)
 *   - auto-indent, bracket auto-close/skip/match-highlight
 *   - line ops: move, duplicate, cut, toggle comment, block (de)indent
 *   - bracketed paste, run-a-shell-command
 *   - mouse: click tabs, tree and text, drag to select, wheel to scroll
 *   - read-only PDF viewer: the rendered page where the terminal can show
 *     images, extracted text everywhere else (v switches), sized to fill the
 *     pane's width, with +/- zoom and arrow-key scrolling
 *   - markdown viewer: Alt+M swaps a .md file between its source and a
 *     rendered view — headings, lists, tables, quotes, code — laid out for
 *     the width of the pane it is shown in
 *   - live diagnostics: C and C++ compile errors and warnings from clangd,
 *     shown in a gap under the line they belong to, updated as you type
 *
 * The mod key is Alt for app-level things; editing chords follow VS Code
 * where the terminal allows (see Alt+H in the app for the full list).
 * App-level keys are remappable in the config.
 *
 * Build:   make                 (tree-sitter support when the library is found)
 *          make TREESITTER=0    (the built-in lexer only)
 *   ./install.sh builds and installs to ~/.local.
 *
 * Source layout — one directory per feature, each with a public header
 * (<dir>/<dir>.h) and, where its files share more than that, a private
 * <dir>/<dir>_internal.h that nothing outside the directory includes:
 *
 *   core/      shared types (Buf, Line, Lang, Node), globals, helpers
 *   editor/    buffer storage, undo, movement, editing, clipboard, completion
 *   syntax/    language table, keyword lexer, tree-sitter highlighting
 *   ui/        tabs, panes, drawing, themes, dialogs
 *   input/     key decoding, keybindings, mouse
 *   tree/      file tree sidebar, new/delete entries
 *   git/       git status markers
 *   terminal/  embedded pty terminal
 *   pdf/       PDF text extraction, page rasterizing, kitty graphics
 *   markdown/  markdown rendering
 *   lsp/       language server client (clangd) and its diagnostics
 *   config/    config and theme files
 *   commands/  prompt, find/replace, quick open, run, app actions
 *   main.c     startup and the key dispatch loop
 *
 * Run:     ./sds [directory]
 *          ./sds --fetch-grammar cpp     install a tree-sitter grammar
 *          ./sds --md-text NOTES.md 80   dump the markdown render as text
 *
 * Diagnostics need clangd on PATH (the clang package) and, for a project's
 * own include paths and flags, a compile_commands.json or compile_flags.txt
 * it can find; without one it falls back to guessing. `[lsp] enabled = off`
 * turns the whole thing off.
 *
 * Tree-sitter is strictly optional and loaded at runtime: grammars live in
 * ~/.local/share/sds/grammars with their highlight queries beside them in
 * ~/.local/share/sds/queries. Any language without an installed grammar —
 * or the whole editor, when built without -DSDS_TREESITTER — falls back to
 * the built-in keyword lexer, which handles every language listed above.
 *
 * Known simplifications: editing is byte-based, so while the cursor will
 * not split a multi-byte character, wide (CJK) glyphs still count as one
 * column and can shift the rendering of a line; no multi-cursor; no LSP.
 * sds itself takes the mouse, but programs running inside a terminal tab
 * cannot: that emulator implements neither mouse reporting nor sixel.
 */


#include "core/sds.h"
#include "input/input.h"
#include "syntax/syntax.h"
#include "editor/editor.h"
#include "pdf/pdf.h"
#include "markdown/markdown.h"
#include "ui/ui.h"
#include "tree/tree.h"
#include "git/git.h"
#include "terminal/terminal.h"
#include "config/config.h"
#include "commands/commands.h"
#include "lsp/lsp.h"

int main(int argc, char **argv) {
    setlocale(LC_ALL, "");
    if (argc > 1 && !strcmp(argv[1], "--fetch-grammar")) {
        if (argc < 3) { fprintf(stderr, "usage: sds --fetch-grammar <lang>\n"); return 1; }
        return fetch_grammar(argv[2]);
    }
    /* Dump a PDF's text and exit — the same extraction the viewer shows, but
     * pipeable, and the handle to debug a document that renders oddly. */
    if (argc > 1 && !strcmp(argv[1], "--pdf-text")) {
        if (argc < 3) { fprintf(stderr, "usage: sds --pdf-text <file.pdf>\n"); return 1; }
        Buf *b = pdf_load(argv[2]);
        if (!b) { fprintf(stderr, "sds: can't read %s\n", argv[2]); return 1; }
        for (int pg = 0; pg < b->pdf->npg; pg++) {
            if (pg) pdf_page_into(b, pg);
            printf("=== page %d/%d ===\n", pg + 1, b->pdf->npg);
            for (int i = 0; i < b->n; i++)
                printf("%.*s\n", b->ln[i].len, b->ln[i].s);
        }
        buf_free(b);
        return 0;
    }
    /* The same rendering the markdown view shows, as plain text at a chosen
     * width — the handle to debug a document that lays out oddly. */
    if (argc > 1 && !strcmp(argv[1], "--md-text")) {
        if (argc < 3) { fprintf(stderr, "usage: sds --md-text <file.md> [width]\n"); return 1; }
        Buf *b = buf_load(argv[2]);
        if (!b) { fprintf(stderr, "sds: can't read %s\n", argv[2]); return 1; }
        b->kind = TAB_FILE;
        int w = argc > 3 ? atoi(argv[3]) : 80;
        md_render(b, w < 12 ? 12 : w);
        for (int i = 0; i < b->md->n; i++)
            printf("%.*s\n", b->md->ln[i].len, b->md->ln[i].s);
        buf_free(b);
        return 0;
    }
    /* What the language server reports for a file, printed once it has
     * answered — the same diagnostics the editor shows under each line. */
    if (argc > 1 && !strcmp(argv[1], "--diagnostics")) {
        if (argc < 3) { fprintf(stderr, "usage: sds --diagnostics <file>\n"); return 1; }
        cfg_load();
        Buf *b = buf_load(argv[2]);
        if (!b) { fprintf(stderr, "sds: can't read %s\n", argv[2]); return 1; }
        b->kind = TAB_FILE;
        lsp_attach(b);
        if (lsp_doc_state(b) < 0) {
            fprintf(stderr, "sds: no language server for %s%s%s\n", argv[2],
                    msg[0] ? " — " : "", msg);
            buf_free(b);
            return 1;
        }
        long end = lsp_now_ms() + 60000;
        while (lsp_doc_state(b) == 0 && lsp_now_ms() < end) {
            lsp_tick();
            struct timespec ts = { 0, 20 * 1000000L };
            nanosleep(&ts, NULL);
        }
        int st = lsp_doc_state(b), count = 0;
        static const char *sev[] = { "", "error", "warning", "info", "hint" };
        for (int li = 0; li < b->n; li++) {
            const Diag *d;
            int n = lsp_line_diags(b, li, &d);
            for (int k = 0; k < n; k++, count++) {
                /* clangd hangs its notes off the message; the editor shows
                 * the first line, so keep that shape and indent the rest */
                int head = (int)strcspn(d[k].msg, "\n");
                printf("%s:%d:%d: %s: %.*s\n", argv[2], d[k].line + 1,
                       d[k].col + 1, sev[d[k].severity], head, d[k].msg);
                for (const char *p = d[k].msg + head; *p; ) {
                    while (*p == '\n') p++;
                    int l = (int)strcspn(p, "\n");
                    if (l) printf("        %.*s\n", l, p);
                    p += l;
                }
            }
        }
        if (st != 1) fprintf(stderr, "sds: no answer from the language server\n");
        else if (!count) printf("no diagnostics\n");
        buf_free(b);
        lsp_shutdown();
        return st == 1 ? 0 : 1;
    }
    if (argc > 1 && (!strcmp(argv[1], "--version") || !strcmp(argv[1], "-v"))) {
        printf("sds (SimpleDevSuite)  tree-sitter: %s\n",
#ifdef SDS_TREESITTER
               "enabled"
#else
               "not built in"
#endif
        );
        return 0;
    }
    const char *dir = argc > 1 ? argv[1] : ".";
    char rp[PATH_MAX];
    if (!realpath(dir, rp)) { fprintf(stderr, "sds: bad path: %s\n", dir); return 1; }
    struct stat st;
    if (stat(rp, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "sds: not a directory: %s\n", rp);
        return 1;
    }
    if (chdir(rp) != 0) { /* non-fatal */ }

    cfg_load();
    kw_index_build();
    hist_load();

    root = node_new("", rp, 1, NULL);
    node_load(root);
    tree_rebuild();
    git_refresh();

    initscr();
    raw();
    noecho();
    keypad(stdscr, TRUE);
    set_escdelay(25);
    {
        const char dirs[6] = { 'A', 'B', 'D', 'C', 'H', 'F' };
        const int  dmap[6] = { D_UP, D_DOWN, D_LEFT, D_RIGHT, D_HOME, D_END };
        char seq[24];
        for (int mod = 2; mod <= 8; mod++)
            for (int d = 0; d < 6; d++) {
                snprintf(seq, sizeof seq, "\033[1;%d%c", mod, dirs[d]);
                define_key(seq, MK(mod, dmap[d]));
            }
        define_key("\033[3;3~", K_ADEL);
        define_key("\033[2;3~", K_AINS);
        define_key("\033[200~", K_PSTART);
        define_key("\033[201~", K_PEND);
    }
    printf("\033[?2004h");                       /* bracketed paste on */
    /* Ask for xterm's modifyOtherKeys level 1: keys that already have a
     * legacy encoding keep it, and only the ambiguous ones — Shift+Enter,
     * which sds binds — arrive as CSI 27;mod;code~. Terminals that don't
     * know the sequence ignore it. */
    printf("\033[>4;1m");
    mouse_enable(1);
    fflush(stdout);

    if (has_colors()) {
        start_color();
        use_default_colors();
        apply_theme();
    }
    if (tree_autohide > 0 && COLS < tree_autohide) tree_hidden = 1;
    if (cfg_warn[0]) set_msg("%s", cfg_warn);

    int need_draw = 1;
    for (;;) {
        /* With a live shell the loop must not block in getch(), or its output
         * would only appear when a key is pressed. Poll instead, and redraw
         * only when something actually changed. */
        int live = any_live_term();
        /* a language server answers whenever it is done, not when a key is
         * pressed, so it needs polling too — just not as eagerly as a shell */
        int poll = lsp_poll_ms(), tsp = ts_pending_ms();
        if (tsp >= 0) poll = poll < 0 ? max2(tsp, 1) : min2(poll, max2(tsp, 1));
        /* a page is being got ready in the background */
        if (gfx_busy()) poll = poll < 0 ? 30 : min2(poll, 30);
        g_timeout = live ? 20 : poll;
        timeout(g_timeout);

        if (need_draw) { draw(); need_draw = 0; }
        int c = read_key_raw();
        if (c == ERR) {                       /* poll tick, no key */
            if (pump_all_terms()) need_draw = 1;
            if (lsp_tick()) need_draw = 1;
            if (ts_pending_ms() == 0) need_draw = 1;   /* the parse is due */
            gfx_tick();                       /* render the next page ahead */
            continue;
        }
        lsp_tick();
        if (pump_all_terms()) need_draw = 1;
        need_draw = 1;
        if (c == K_NONE || c == KEY_RESIZE) continue;
        if (show_help) {                  /* any key closes it, but scroll first */
            int wheel = (c == K_MOUSE && mev.press) ? mev.btn : -1;
            if (c == KEY_UP)             { help_scroll(-1, 0); continue; }
            if (c == KEY_DOWN)           { help_scroll( 1, 0); continue; }
            if (wheel == MB_WHEEL_UP)    { help_scroll(-3, 0); continue; }
            if (wheel == MB_WHEEL_DOWN)  { help_scroll( 3, 0); continue; }
            if (c == KEY_PPAGE)          { help_scroll(-1, 1); continue; }
            if (c == KEY_NPAGE)          { help_scroll( 1, 1); continue; }
            if (c == K_MOUSE && (mev.motion || !mev.press)) continue;
            show_help = 0;
            continue;
        }
        /* before the terminal branch below: a click has to be able to reach
         * the tree and the tab bar even while a shell holds the keyboard */
        if (c == K_MOUSE) { handle_mouse(); continue; }

        /* a focused terminal swallows everything except the app-level keys */
        if (cur >= 0 && tabs[cur]->kind == TAB_TERM) {
            Term *t = tabs[cur]->term;
            if (c == MK(2, D_UP) || c == KEY_SPREVIOUS) {      /* Shift+PgUp */
                if (t) t->sb_view = min2(t->sb_n, t->sb_view + t->rows / 2);
                continue;
            }
            if (c == MK(2, D_DOWN) || c == KEY_SNEXT) {
                if (t) t->sb_view = max2(0, t->sb_view - t->rows / 2);
                continue;
            }
            int app = (c == kb[KB_QUIT] || c == kb[KB_CLOSE_TAB] ||
                       c == kb[KB_HELP] || c == kb[KB_TERM] ||
                       c == kb[KB_TAB_PREV] || c == kb[KB_TAB_NEXT] ||
                       c == kb[KB_SIDEBAR] || c == kb[KB_QUICKOPEN] ||
                       c == kb[KB_TREE_UP] || c == kb[KB_TREE_DOWN] ||
                       c == kb[KB_TREE_OPEN] || c == kb[KB_TREE_OPEN_PANE] ||
                       c == kb[KB_TREE_COLLAPSE] ||
                       c == kb[KB_TREE_EXPAND] ||
                       c == kb[KB_PANE_LEFT] || c == kb[KB_PANE_RIGHT] ||
                       c == kb[KB_PANE_UP] || c == kb[KB_PANE_DOWN] ||
                       c == kb[KB_PANE_CLOSE] || IS_PKEY(c) ||
                       (c >= ALT('1') && c <= ALT('9')));
            if (!app) {
                if (t && !t->dead) term_key(t, c);
                else if (t && t->dead && c != K_NONE)
                    set_msg("shell exited — Alt+W closes this tab", NULL);
                continue;
            }
        }

        if (c != kb[KB_CLOSE_TAB]) pending_close = 0;
        if (c != kb[KB_QUIT])      pending_quit = 0;
        msg[0] = 0;

        /* ── app-level ── */
        /* An if-chain rather than a switch: the bindings come from the config
         * at runtime, so they are not case-label constants. The second key on
         * some lines is a fixed alias that has always worked. */
        if (c == kb[KB_TREE_UP])
            { if (tree_active() && tsel > 0) tsel--;        continue; }
        if (c == kb[KB_TREE_DOWN])
            { if (tree_active() && tsel < nvis - 1) tsel++; continue; }
        if (c == kb[KB_TREE_COLLAPSE])   { tree_collapse();             continue; }
        if (c == kb[KB_TREE_EXPAND])     { tree_expand();               continue; }
        if (c == kb[KB_TREE_OPEN_PANE])
            { if (tree_active()) tree_open_pane_selected();             continue; }
        if (c == kb[KB_TREE_OPEN])
            { if (tree_active()) tree_open_selected();                  continue; }
        if (c == kb[KB_DEL_ENTRY])
            { if (tree_active()) tree_delete_selected();                continue; }
        if (c == kb[KB_NEW_ENTRY])
            { if (tree_active()) tree_new_entry();                      continue; }
        if (c == kb[KB_REFRESH] || c == ALT('e')) {
            tree_refresh(); set_msg("tree refreshed", NULL);            continue;
        }
        if (c == kb[KB_TAB_PREV] || c == ALT('[')) {
            if (ntabs) set_cur((cur + ntabs - 1) % ntabs);
            continue;
        }
        if (c == kb[KB_TAB_NEXT] || c == ALT(']')) {
            if (ntabs) set_cur((cur + 1) % ntabs);
            continue;
        }
        if (c == kb[KB_PANE_LEFT])  { pane_focus_dir(D_LEFT);  continue; }
        if (c == kb[KB_PANE_RIGHT]) { pane_focus_dir(D_RIGHT); continue; }
        if (c == kb[KB_PANE_UP])    { pane_focus_dir(D_UP);    continue; }
        if (c == kb[KB_PANE_DOWN])  { pane_focus_dir(D_DOWN);  continue; }
        if (c == kb[KB_PANE_CLOSE]) { pane_close();            continue; }
        if (IS_PKEY(c)) {                        /* Alt+Shift+1..9 → pane N */
            int d = c - PKEY(0);
            if (d >= 1 && d <= 9) {
                if (d - 1 < ntabs) pane_show_tab(d - 1);
                else set_msg("that tab is not open%s", "");
            }
            continue;
        }
        if (c == kb[KB_TERM])            { open_terminal();             continue; }
        if (c == kb[KB_CLOSE_TAB])       { act_close();                 continue; }
        if (c == kb[KB_SAVE] || c == ALT('s')) { act_save();            continue; }
        if (c == kb[KB_HELP])   { show_help = 1; help_off = 0;          continue; }
        if (c == kb[KB_RUN])             { run_command();               continue; }
        if (c == kb[KB_QUIT])            { if (act_quit()) goto done;   continue; }
        if (c == kb[KB_QUICKOPEN])       { do_quickopen();              continue; }
        if (c == kb[KB_SIDEBAR])         { tree_toggle();               continue; }
        if (c == kb[KB_MARKDOWN])        { md_toggle();                 continue; }
        if (c == kb[KB_WRAP]) {
            wrap = !wrap;
            for (int i = 0; i < ntabs; i++) tabs[i]->subrow = 0;
            set_msg(wrap ? "word wrap on" : "word wrap off", NULL);
            continue;
        }
        if (c == kb[KB_LINE_SCROLL]) {
            line_scroll = !line_scroll;
            set_msg(line_scroll ? "sideways scroll: current line only"
                                : "sideways scroll: whole view", NULL);
            continue;
        }
        if (c >= ALT('1') && c <= ALT('9')) {
            focus_tab(c - ALT('1'));
            continue;
        }
        if (c == K_PSTART) { handle_bracketed_paste(); continue; }

        /* ── editor ── */
        if (cur < 0) {
            if (c == kb[KB_FIND] || c == kb[KB_GOTO])
                set_msg("open a file first", NULL);
            continue;
        }
        /* ── PDF: a read-only viewer, so only movement and copying apply ── */
        if (tabs[cur]->kind == TAB_PDF) {
            Buf *b = tabs[cur];
            Pdf *pf = b->pdf;
            if (c == KEY_RIGHT || c == ' ' || c == 'n') {
                if (pf->page + 1 < pf->npg) pdf_page_into(b, pf->page + 1);
                else set_msg("last page%s", "");
                continue;
            }
            if (c == KEY_LEFT || c == 'p') {
                if (pf->page > 0) pdf_page_into(b, pf->page - 1);
                else set_msg("first page%s", "");
                continue;
            }
            if (c == kb[KB_GOTO]) {                       /* Ctrl+G: page number */
                char in[32] = "";
                if (prompt("Page: ", in, sizeof in, NULL, 0) && in[0]) {
                    int n = atoi(in);
                    if (n >= 1 && n <= pf->npg) pdf_page_into(b, n - 1);
                    else set_msg("no such page%s", "");
                }
                continue;
            }
            if (c == 'v') {                          /* page image <-> text */
                if (b->pdf_img) {
                    b->pdf_img = 0;
                    set_msg("showing extracted text — v for the page image%s", "");
                } else {
                    const char *why = pdf_render_why_not();
                    if (why) set_msg("%s", why);
                    else { b->pdf_img = 1; set_msg("showing the page — v for text%s", ""); }
                }
                continue;
            }
            /* On the page image the arrows scroll the picture instead of an
             * invisible text cursor, and +/- resize it. Steps are in cells so
             * they feel the same whatever the zoom. */
            if (b->pdf_img) {
                int cw, chh;
                cell_px(&cw, &chh);
                int screen = max2(chh, pf->view_h - 2 * chh);
                switch (c) {
                    case KEY_UP:         pdf_scroll(b, -3 * chh);  continue;
                    case KEY_DOWN:       pdf_scroll(b,  3 * chh);  continue;
                    case MK(2, D_UP):    pdf_scroll(b, -chh);      continue;
                    case MK(2, D_DOWN):  pdf_scroll(b,  chh);      continue;
                    case KEY_PPAGE:      pdf_scroll(b, -screen);   continue;
                    case KEY_NPAGE:      pdf_scroll(b,  screen);   continue;
                    case KEY_HOME:       pf->sy = 0;               continue;
                    case KEY_END:        pf->sy = PDF_SCROLL_BOTTOM; continue;
                    case MK(2, D_LEFT):  pdf_pan(b, -4 * cw);      continue;
                    case MK(2, D_RIGHT): pdf_pan(b,  4 * cw);      continue;
                    case MK(5, D_HOME):  pdf_page_into(b, 0);      continue;
                    case MK(5, D_END):   pdf_page_into(b, pf->npg - 1); continue;
                    case '+': case '=':  pdf_set_zoom(b, pf->zoom * 1.25); continue;
                    case '-': case '_':  pdf_set_zoom(b, pf->zoom / 1.25); continue;
                    case '0': {
                        char pct[32];
                        pf->sx = pf->sy = 0;
                        pdf_set_zoom(b, cfg_pdf_zoom);
                        snprintf(pct, sizeof pct, "%d%%",
                                 (int)(cfg_pdf_zoom * 100 + 0.5));
                        set_msg("default zoom — %s of the pane width", pct);
                        continue;
                    }
                    case 'f': pdf_fit_page(b); continue;
                    default: break;
                }
            } else if (c == '+' || c == '=' || c == '-' || c == '_' || c == 'f') {
                set_msg("zoom applies to the page image — press v%s", "");
                continue;
            }
            if (c == kb[KB_FIND])      { do_find();   continue; }
            if (c == kb[KB_FIND_NEXT]) { find_next(); continue; }
            switch (c) {
                case KEY_UP:    move_cursor(b, M_UP, 0);    break;
                case KEY_DOWN:  move_cursor(b, M_DOWN, 0);  break;
                case KEY_HOME:  move_cursor(b, M_HOME, 0);  break;
                case KEY_END:   move_cursor(b, M_END, 0);   break;
                case KEY_PPAGE: move_cursor(b, M_PGUP, 0);  break;
                case KEY_NPAGE: move_cursor(b, M_PGDN, 0);  break;
                case MK(2, D_UP):    move_cursor(b, M_UP, 1);    break;
                case MK(2, D_DOWN):  move_cursor(b, M_DOWN, 1);  break;
                case MK(2, D_LEFT):  move_cursor(b, M_LEFT, 1);  break;
                case MK(2, D_RIGHT): move_cursor(b, M_RIGHT, 1); break;
                case MK(5, D_HOME):  move_cursor(b, M_DOCHOME, 0); break;
                case MK(5, D_END):   move_cursor(b, M_DOCEND, 0);  break;
                case CTRL('c'):      ed_copy(b, 0); break;
                case CTRL('a'):
                    b->sel = 1; b->ay = 0; b->ax = 0;
                    b->cy = b->n - 1; b->cx = b->ln[b->cy].len;
                    break;
                case 27: b->sel = 0; find_show = 0; break;
                default: break;
            }
            continue;
        }
        /* ── rendered markdown: read-only, so it keeps only the keys that
         * move the page and hands nothing through to the editor ── */
        if (tabs[cur]->kind == TAB_FILE && tabs[cur]->md_view) {
            Buf *b = tabs[cur];
            int rows = max2(1, focused_pane_rows() - 1);
            switch (c) {
                case KEY_UP:         md_scroll(b, -1);        break;
                case KEY_DOWN:       md_scroll(b,  1);        break;
                case KEY_PPAGE:      md_scroll(b, -rows);     break;
                case ' ':
                case KEY_NPAGE:      md_scroll(b,  rows);     break;
                case KEY_HOME:
                case MK(5, D_HOME):  md_scroll(b, -INT_MAX);  break;
                case KEY_END:
                case MK(5, D_END):   md_scroll(b,  INT_MAX);  break;
                default:
                    if (c == kb[KB_FIND] || c == kb[KB_FIND_NEXT] ||
                        c == kb[KB_REPLACE] || c == kb[KB_GOTO])
                        set_msg("that works in the source — Alt+M%s", "");
                    break;
            }
            continue;
        }
        if (tabs[cur]->kind != TAB_FILE) continue;   /* terminals handled above */
        Buf *b = tabs[cur];
        /* configurable editor actions, again as an if-chain */
        if (c == kb[KB_FIND])      { do_find();     continue; }
        if (c == kb[KB_FIND_NEXT]) { find_next();   continue; }
        if (c == kb[KB_REPLACE])   { do_replace();  continue; }
        if (c == kb[KB_GOTO])      { do_goto();     continue; }
        if (c == kb[KB_COMPLETE])  { do_complete(); continue; }
        if (c == kb[KB_MOVE_UP])   { ed_move_lines(b, 0); continue; }
        if (c == kb[KB_MOVE_DOWN]) { ed_move_lines(b, 1); continue; }
        switch (c) {
            /* movement */
            case KEY_UP:        move_cursor(b, M_UP, 0);      break;
            case KEY_DOWN:      move_cursor(b, M_DOWN, 0);    break;
            case KEY_LEFT:      move_cursor(b, M_LEFT, 0);    break;
            case KEY_RIGHT:     move_cursor(b, M_RIGHT, 0);   break;
            case KEY_HOME:      move_cursor(b, M_HOME, 0);    break;
            case KEY_END:       move_cursor(b, M_END, 0);     break;
            case KEY_PPAGE:     move_cursor(b, M_PGUP, 0);    break;
            case KEY_NPAGE:     move_cursor(b, M_PGDN, 0);    break;
            case MK(2, D_UP):    move_cursor(b, M_UP, 1);     break;
            case MK(2, D_DOWN):  move_cursor(b, M_DOWN, 1);   break;
            case MK(2, D_LEFT):  move_cursor(b, M_LEFT, 1);   break;
            case MK(2, D_RIGHT): move_cursor(b, M_RIGHT, 1);  break;
            case MK(2, D_HOME):  move_cursor(b, M_HOME, 1);   break;
            case MK(2, D_END):   move_cursor(b, M_END, 1);    break;
            case MK(5, D_LEFT):  move_cursor(b, M_WORDL, 0);  break;
            case MK(5, D_RIGHT): move_cursor(b, M_WORDR, 0);  break;
            case MK(6, D_LEFT):  move_cursor(b, M_WORDL, 1);  break;
            case MK(6, D_RIGHT): move_cursor(b, M_WORDR, 1);  break;
            case MK(5, D_HOME):  move_cursor(b, M_DOCHOME, 0); break;
            case MK(5, D_END):   move_cursor(b, M_DOCEND, 0);  break;
            case MK(6, D_HOME):  move_cursor(b, M_DOCHOME, 1); break;
            case MK(6, D_END):   move_cursor(b, M_DOCEND, 1);  break;
            case MK(5, D_UP):                       /* scroll viewport */
                ed_scroll(b, 0, 1, focused_pane_rows());
                break;
            case MK(5, D_DOWN):
                ed_scroll(b, 1, 1, focused_pane_rows());
                break;
            /* editing */
            case '\r': case '\n': case KEY_ENTER: ed_enter(b);     break;
            case KEY_BACKSPACE: case 127: case 8: ed_backspace(b); break;
            case KEY_DC:                          ed_delete(b);    break;
            case '\t':                            ed_tab(b, 0);    break;
            case KEY_BTAB:                        ed_tab(b, 1);    break;
            case CTRL('z'):                       do_undo(b);      break;
            case CTRL('y'):                       do_redo(b);      break;
            case CTRL('c'):                       ed_copy(b, 0);   break;
            case CTRL('x'):                       ed_copy(b, 1);   break;
            case CTRL('v'):
                if (clip) ed_paste_text(b, clip, cliplen);
                else set_msg("clipboard empty (use Ctrl+Shift+V for terminal paste)", NULL);
                break;
            case CTRL('a'):
                b->sel = 1; b->ay = 0; b->ax = 0;
                b->cy = b->n - 1; b->cx = b->ln[b->cy].len;
                break;
            case CTRL('d'):                       ed_dup_line(b);  break;
            /* Ctrl+/ is the one every editor agrees on, but plenty of layouts
             * make it awkward to reach, so Ctrl+K does the same thing. */
            case CTRL('k'): case 31: /* Ctrl+/ */ ed_toggle_comment(b); break;
            case ALT('o'):                        ed_open_below(b); break;
            case 27:
                b->sel = 0; find_show = 0;
                break;
            default:
                if (c >= 32 && c != 127 && c < 256) ed_type(b, c);
        }
    }
done:
    lsp_shutdown();
    gfx_clear_all();            /* do not leave images behind in the terminal */
    tc_restore();               /* leave the terminal's palette as we found it */
    refresh();
    printf("\033[?2004l");
    printf("\033[>4;0m");       /* and hand the key encoding back as we found it */
    mouse_enable(0);
    fflush(stdout);
    endwin();
    return 0;
}
