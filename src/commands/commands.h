/* commands.h — prompt, find, quick open and app actions */
#ifndef SDS_COMMANDS_H
#define SDS_COMMANDS_H

#include "core/sds.h"

/* prompt.c */
void hist_load(void);
int prompt(const char *label, char *out, size_t cap,
                  void (*live)(const char *), int use_hist);

/* find.c */
extern char findq[256];
extern int find_show;
void do_find(void);
void find_next(void);
void do_replace(void);
void do_goto(void);

/* quickopen.c */
void do_quickopen(void);

/* run.c */
void run_command(void);

/* actions.c */
void md_toggle(void);
void md_scroll(Buf *b, int dy);
void act_save(void);
void act_close(void);
int act_quit(void);

#endif /* SDS_COMMANDS_H */
