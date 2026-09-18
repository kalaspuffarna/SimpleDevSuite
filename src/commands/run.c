#include "core/sds.h"
#include "commands/commands.h"
#include "commands/commands_internal.h"
#include "input/input.h"
#include "editor/editor.h"
#include "ui/ui.h"
#include "tree/tree.h"

/* ── run command ──────────────────────────────────────────────────── */
void run_command(void) {
    char in[512] = "";              /* always start empty; Up recalls history */
    if (!prompt("Run: ", in, sizeof in, NULL, 1)) return;
    if (!in[0]) return;
    hist_add(in);
    hist_save();

    int saved = 0;                    /* compile what's on screen, not on disk */
    for (int i = 0; i < ntabs; i++)
        if (tabs[i]->dirty && buf_save(tabs[i]) == 0) saved++;

    def_prog_mode();
    tc_restore();                       /* the command gets the real palette */
    endwin();
    printf("\033[?2004l");
    printf("\033[>4;0m");               /* the command gets plain key encoding */
    mouse_enable(0);                    /* and the mouse back, for its own use */
    printf("\033[H\033[2J\033[3J");     /* clear screen + scrollback */
    if (saved) printf("[saved %d file(s)]\n", saved);
    printf("$ %s\n", in);
    fflush(stdout);

    int st = system(in);
    /* system() hands back a wait status, not an exit code */
    if (st == -1)              printf("\n[could not run]");
    else if (WIFSIGNALED(st))  printf("\n\033[31m[killed by signal %d]\033[0m",
                                      WTERMSIG(st));
    else if (WEXITSTATUS(st))  printf("\n\033[31m[exit %d]\033[0m", WEXITSTATUS(st));
    else                       printf("\n\033[32m[exit 0]\033[0m");
    printf(" — press any key ");
    fflush(stdout);

    reset_prog_mode();
    apply_theme();                    /* and sds takes its palette back */
    printf("\033[?2004h");
    printf("\033[>4;1m");
    mouse_enable(1);
    fflush(stdout);
    getch();                          /* any key, not just Enter */

    tree_refresh();                   /* pick up a.out, build/, generated files */
    refresh();
}
