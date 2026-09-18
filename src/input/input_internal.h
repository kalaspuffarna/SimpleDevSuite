/* input_internal.h — key codes, key reading and mouse (shared between the files in input/ only) */
#ifndef SDS_INPUT_INTERNAL_H
#define SDS_INPUT_INTERNAL_H

#include "core/sds.h"
#include "input/input.h"

/* keybindings.c */
int kb_digit_of(const char *ch, int len);

#endif /* SDS_INPUT_INTERNAL_H */
