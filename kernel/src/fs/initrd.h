#ifndef NEXUS_INITRD_H
#define NEXUS_INITRD_H

#include "klib/klib.h"

/* Unpacks a USTAR tar archive (as produced by `tar --format=ustar`,
 * which is exactly how the build packages userland/bin/ -- see the top
 * level GNUmakefile's `initrd.tar` target) living at `data` (length
 * `size`) into the already-mounted VFS root, creating directories and
 * files as needed. Returns the number of regular files created. */
uint32_t initrd_unpack(const void *data, size_t size);

#endif /* NEXUS_INITRD_H */
