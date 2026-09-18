/* terminal_internal.h — embedded terminal emulator (shared between the files in terminal/ only) */
#ifndef SDS_TERMINAL_INTERNAL_H
#define SDS_TERMINAL_INTERNAL_H

#include "core/sds.h"
#include "terminal/terminal.h"

/* emulator.c */
Cell term_blank(Term *t);
void term_feed(Term *t, const char *s, int n);

#endif /* SDS_TERMINAL_INTERNAL_H */
