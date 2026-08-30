#ifndef NEXUS_GRAPHFS_VFS_H
#define NEXUS_GRAPHFS_VFS_H

#include "fs/vfs.h"

// Root vnode for the graph filesystem's classic-VFS adapter -- see graphfs_vfs.c.
struct vnode *graphfs_vfs_root(void);

#endif /* NEXUS_GRAPHFS_VFS_H */
