/* tree_internal.h — file tree sidebar (shared between the files in tree/ only) */
#ifndef SDS_TREE_INTERNAL_H
#define SDS_TREE_INTERNAL_H

#include "core/sds.h"
#include "tree/tree.h"

/* tree.c */
int node_cmp(const void *a, const void *b);
void node_free(Node *n);

#endif /* SDS_TREE_INTERNAL_H */
