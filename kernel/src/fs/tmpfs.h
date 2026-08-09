#ifndef NEXUS_TMPFS_H
#define NEXUS_TMPFS_H

#include "fs/vfs.h"

/* Creates a fresh, empty tmpfs and returns its root directory vnode,
 * ready to hand to vfs_set_root(). Everything lives in kmalloc'd
 * memory -- there's no backing store, so contents don't survive a
 * reboot (hence "tmp"). This is Nexus's only filesystem so far; the
 * initrd gets unpacked into one at boot (see fs/initrd.h) and that's
 * what `/` actually is. */
struct vnode *tmpfs_create_root(void);

#endif /* NEXUS_TMPFS_H */
