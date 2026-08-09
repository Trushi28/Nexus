#ifndef NEXUS_HEAP_H
#define NEXUS_HEAP_H

#include "klib/klib.h"

/* Seeds the heap with an initial chunk of pages. Call once, after the
 * PMM and VMM are both up (allocations are served straight out of the
 * direct map, so no dedicated VMM region is needed). */
void heap_init(void);

void *kmalloc(size_t size);
void  kfree(void *ptr);
void *kzalloc(size_t size);

uint64_t heap_used_bytes(void);
uint64_t heap_capacity_bytes(void);

#endif /* NEXUS_HEAP_H */
