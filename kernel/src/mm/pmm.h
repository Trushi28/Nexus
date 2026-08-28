#ifndef NEXUS_PMM_H
#define NEXUS_PMM_H

#include "klib/klib.h"

// Initializes the physical page allocator from the boot memory map.Must run before any PMM allocation.
void pmm_init(void);

//Returns the physical address of a freshly zeroed 4KiB frame, or 0 if out of memory.
uint64_t pmm_alloc_page(void);

// Returns the physical address of `count` *contiguous* zeroed 4KiB frames, or 0 if no run of that length is free.
uint64_t pmm_alloc_pages(size_t count);

void pmm_free_page(uint64_t phys);
void pmm_free_pages(uint64_t phys, size_t count);

uint64_t pmm_total_bytes(void);
uint64_t pmm_used_bytes(void);
uint64_t pmm_free_bytes(void);

#endif /* NEXUS_PMM_H */
