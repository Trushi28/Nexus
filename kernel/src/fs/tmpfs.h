#ifndef NEXUS_TMPFS_H
#define NEXUS_TMPFS_H

#include "fs/vfs.h"

// Fresh, empty, in-memory-only tmpfs -- doesn't survive a reboot.
struct vnode *tmpfs_create_root(void);

#endif /* NEXUS_TMPFS_H */
