/* tree.h — file tree sidebar */
#ifndef SDS_TREE_H
#define SDS_TREE_H

#include "core/sds.h"

/* tree.c */
extern int tree_w;
extern int tree_autohide;
extern int tree_hidden;
extern Node *root;
extern Node **vis;
extern int nvis, viscap, tsel, toff;
Node *node_new(const char *name, const char *path, int is_dir, Node *parent);
void node_load(Node *d);
int tree_active(void);
void tree_rebuild(void);
void tree_open_selected(void);
void tree_open_pane_selected(void);
void tree_toggle(void);
void tree_collapse(void);
void tree_refresh(void);
void tree_expand(void);

/* tree_edit.c */
void tree_new_entry(void);
void tree_delete_selected(void);

#endif /* SDS_TREE_H */
