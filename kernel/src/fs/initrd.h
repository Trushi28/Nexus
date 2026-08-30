#ifndef NEXUS_INITRD_H
#define NEXUS_INITRD_H

#include "klib/klib.h"

/* Unpacks a USTAR archive (as `tar --format=ustar` produces) into the
 * mounted VFS root. Returns the number of regular files created. */
uint32_t initrd_unpack(const void *data, size_t size);

#endif /* NEXUS_INITRD_H */
