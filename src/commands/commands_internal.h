/* commands_internal.h — prompt, find, quick open and app actions (shared between the files in commands/ only) */
#ifndef SDS_COMMANDS_INTERNAL_H
#define SDS_COMMANDS_INTERNAL_H

#include "core/sds.h"
#include "commands/commands.h"

/* prompt.c */
void hist_add(const char *s);
void hist_save(void);

#endif /* SDS_COMMANDS_INTERNAL_H */
