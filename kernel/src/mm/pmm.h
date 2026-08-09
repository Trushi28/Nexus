#ifndef NEXUS_PMM_H
#define NEXUS_PMM_H

#include "klib/klib.h"

/* Builds the frame bitmap from the Limine memory map. Must run before
 * anything calls pmm_alloc_page(). Physical pages are accessed through
 * the bootloader's HHDM up until vmm_init() switches to our own page
 * tables (which re-establish the same direct map, so nothing breaks). */
void pmm_init(void);

/* Returns the physical address of a freshly zeroed 4KiB frame, or 0 if
 * out of memory. */
uint64_t pmm_alloc_page(void);

/* Returns the physical address of `count` *contiguous* zeroed 4KiB
 * frames, or 0 if no run of that length is free. */
uint64_t pmm_alloc_pages(size_t count);

void pmm_free_page(uint64_t phys);
void pmm_free_pages(uint64_t phys, size_t count);

uint64_t pmm_total_bytes(void);
uint64_t pmm_used_bytes(void);
uint64_t pmm_free_bytes(void);

#endif /* NEXUS_PMM_H */
