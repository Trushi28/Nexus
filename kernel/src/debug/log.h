#ifndef NEXUS_LOG_H
#define NEXUS_LOG_H

#include "klib/klib.h"

__attribute__((format(printf, 1, 2))) int kprintf(const char *fmt, ...);

__attribute__((format(printf, 1, 2))) int kprintf_locked(const char *fmt, ...);

uint64_t kprintf_lock_acquire(void);
void kprintf_lock_release(uint64_t flags);

#endif /* NEXUS_LOG_H */
