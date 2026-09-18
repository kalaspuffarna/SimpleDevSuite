/* git.h — git status */
#ifndef SDS_GIT_H
#define SDS_GIT_H

#include "core/sds.h"

/* git.c */
extern int git_repo;
extern char git_branch[128];
void git_refresh(void);
char git_status_for(const char *abs, int is_dir);

#endif /* SDS_GIT_H */
