#ifndef NEXUS_HEAP_H
#define NEXUS_HEAP_H

#include "klib/klib.h"

// Initializes the kernel heap. Call after PMM and VMM initialization.
void heap_init(void);

void *kmalloc(size_t size);
void  kfree(void *ptr);
void *kzalloc(size_t size);

uint64_t heap_used_bytes(void);
uint64_t heap_capacity_bytes(void);

#endif /* NEXUS_HEAP_H */
