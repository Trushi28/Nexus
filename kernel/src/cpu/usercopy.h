#ifndef NEXUS_USERCOPY_H
#define NEXUS_USERCOPY_H

#include "klib/klib.h"

/* Real, page-fault-safe copies to/from a user-supplied virtual address.
 * Unlike the shallow "is this pointer plausible" check in
 * user_range_ok() (cpu/syscall.c), these survive a syntactically-valid-
 * but-unmapped user pointer: a fault partway through is caught by the
 * exception-table hook in cpu/isr.c and turned into an ordinary error
 * return instead of an unhandled #PF/panic. Still call user_range_ok()
 * first for the canonical-address/NULL-guard/kernel-half checks -- these
 * functions only protect against *not-present* pages within an
 * otherwise plausible range, not against a pointer aimed at kernel
 * space (which IS present, and would just leak/corrupt kernel memory
 * without a fault to catch).
 *
 * copy_from_user()/copy_to_user() return the number of bytes that could
 * NOT be copied (0 on full success). copy_string_from_user() returns
 * false on a fault before finding a NUL terminator.
 */
size_t copy_from_user(void *kdst, const void *usrc, size_t len);
size_t copy_to_user(void *udst, const void *ksrc, size_t len);

/* Copies a NUL-terminated string of at most `max` bytes (including the
 * NUL) from user memory into kdst, always NUL-terminating kdst on
 * success. Byte-at-a-time on top of copy_from_user() rather than one
 * bulk copy -- there's no "rep movs until a zero byte" instruction, and
 * a bulk copy of `max` bytes from a short string that happens to sit
 * right at the edge of a mapped region would fault on bytes past the
 * string we never actually needed. Fine perf-wise: this is for path
 * strings, not a hot data path. */
bool copy_string_from_user(char *kdst, const void *usrc, size_t max);

#endif /* NEXUS_USERCOPY_H */
