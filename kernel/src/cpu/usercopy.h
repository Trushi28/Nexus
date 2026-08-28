#ifndef NEXUS_USERCOPY_H
#define NEXUS_USERCOPY_H

#include "klib/klib.h"

/*
 * Fault-safe copies to and from user memory.
 *
 * Callers must validate address ranges separately with user_range_ok().
 * Returns the number of bytes not copied (0 on success).
 */
size_t copy_from_user(void *kdst, const void *usrc, size_t len);
size_t copy_to_user(void *udst, const void *ksrc, size_t len);

/*
 * Copies a NUL-terminated string from user memory, up to `max` bytes.
 * Stops at the terminator to avoid accessing unmapped bytes beyond it.
 * Returns false if a fault occurs before the terminator.
 */
bool copy_string_from_user(char *kdst, const void *usrc, size_t max);

#endif /* NEXUS_USERCOPY_H */
