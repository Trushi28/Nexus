#ifndef NEXUS_VMM_H
#define NEXUS_VMM_H

#include "klib/klib.h"

#define VMM_PRESENT (1ULL << 0)
#define VMM_WRITABLE (1ULL << 1)
#define VMM_USER (1ULL << 2)
#define VMM_PWT (1ULL << 3)
#define VMM_PCD (1ULL << 4)
#define VMM_NX (1ULL << 63)

/* Map only physical regions reported by the boot memory map. Mapping every
 * address up to the highest physical address can waste page tables on gaps. */
void vmm_init(void);

uint64_t vmm_kernel_pml4_phys(void);
void vmm_load_kernel_pagemap(void);

/* Maps a single 4KiB page. Allocates any missing intermediate page
 * tables from the PMM. `flags` is a combination of the VMM_* bits above
 * (VMM_PRESENT is implied/always set). */
void vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags);
void vmm_map_range(uint64_t virt, uint64_t phys, uint64_t size, uint64_t flags);

uint64_t vmm_new_address_space(void);

/* Allocates a new address space with the kernel upper half pre-populated.
 * Returns its physical PML4 address, or 0 on OOM. */
uint64_t vmm_copy_address_space(uint64_t src_pml4_phys);

// Maps into the address space identified by `pml4_phys`, regardless of the currently loaded CR3.
void vmm_map_page_in(uint64_t pml4_phys, uint64_t virt, uint64_t phys, uint64_t flags);
void vmm_map_range_in(uint64_t pml4_phys, uint64_t virt, uint64_t phys, uint64_t size, uint64_t flags);

// Removes a mapping, invalidates it locally, and frees its physical frame.Empty intermediate page tables are retained.
void vmm_unmap_page_in(uint64_t pml4_phys, uint64_t virt);
void vmm_unmap_range_in(uint64_t pml4_phys, uint64_t virt, uint64_t size);

// Frees all lower-half user mappings and page tables, then the PML4 itself.The shared kernel upper half is preserved.
void vmm_free_user_space(uint64_t pml4_phys);

/ Maps a physical MMIO range as uncacheable and returns a pointer to it.MMIO mappings are permanent.
void *vmm_map_mmio(uint64_t phys, size_t size);

// Invalidates `virt`, or the entire TLB when `virt` is 0, on all online CPUs.
void vmm_flush_tlb_all_cpus(uint64_t virt);

#endif /* NEXUS_VMM_H */
