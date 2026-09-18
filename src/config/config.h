/* config.h — config file loading */
#ifndef SDS_CONFIG_H
#define SDS_CONFIG_H

#include "core/sds.h"

/* config.c */
extern char cfg_warn[256];
void cfg_load(void);

#endif /* SDS_CONFIG_H */
