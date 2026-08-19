#ifndef NEXUS_GRAPHFS_VFS_H
#define NEXUS_GRAPHFS_VFS_H

#include "fs/vfs.h"

/* Returns the singleton root vnode for the graph filesystem's
 * classic-VFS adapter -- hand this to vfs_mount() to graft the graph
 * (fs/graph.c) into the ordinary path namespace (see graphfs_vfs.c
 * for exactly how each vnode_ops entry maps onto the graph's own
 * node/edge/sstring API). The g*-prefixed shell commands keep working
 * unchanged -- both interfaces operate on the exact same underlying
 * nodes. */
struct vnode *graphfs_vfs_root(void);

#endif /* NEXUS_GRAPHFS_VFS_H */
