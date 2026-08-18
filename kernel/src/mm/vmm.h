#ifndef NEXUS_VMM_H
#define NEXUS_VMM_H

#include "klib/klib.h"

#define VMM_PRESENT   (1ULL << 0)
#define VMM_WRITABLE  (1ULL << 1)
#define VMM_USER      (1ULL << 2)
#define VMM_PWT       (1ULL << 3)
#define VMM_PCD       (1ULL << 4)
#define VMM_NX        (1ULL << 63)

/* Builds Nexus's own kernel page tables (direct map covering all of
 * physical memory + the kernel image mapped with correct W^X
 * permissions), then switches CR3 to them. Call once, on the BSP, after
 * pmm_init(). Every AP just needs vmm_load_kernel_pagemap(). */
void vmm_init(void);

uint64_t vmm_kernel_pml4_phys(void);
void     vmm_load_kernel_pagemap(void);

/* Maps a single 4KiB page. Allocates any missing intermediate page
 * tables from the PMM. `flags` is a combination of the VMM_* bits above
 * (VMM_PRESENT is implied/always set). */
void vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags);
void vmm_map_range(uint64_t virt, uint64_t phys, uint64_t size, uint64_t flags);

/* --------------------------- address spaces --------------------------
 * Per-process (ring-3) page tables. A new address space starts with the
 * kernel's own upper-half mapping (direct map, kernel image, MMIO
 * window) shared wholesale -- see vmm_new_address_space()'s comment --
 * and an empty lower half, ready for vmm_map_page_in()/elf_load() to
 * populate with one process's own private mappings.
 * ---------------------------------------------------------------------- */

/* Allocates a fresh PML4 with the kernel's upper half pre-populated.
 * Returns its physical address, or 0 on OOM. */
uint64_t vmm_new_address_space(void);

/* Like vmm_map_page()/vmm_map_range(), but targets an arbitrary address
 * space by physical PML4 address rather than the kernel's own. Safe to
 * call regardless of which CR3 is currently loaded -- page tables are
 * always walked through the direct map. */
void vmm_map_page_in(uint64_t pml4_phys, uint64_t virt, uint64_t phys, uint64_t flags);
void vmm_map_range_in(uint64_t pml4_phys, uint64_t virt, uint64_t phys, uint64_t size, uint64_t flags);

/* Unmaps a single page (if one is mapped there -- a no-op otherwise)
 * from the given address space: clears the leaf PTE, invalidates it
 * locally, and frees the physical frame it pointed at back to the
 * PMM. Does NOT reclaim now-possibly-empty intermediate page-table
 * levels (PT/PD/PDPT) -- same "keep it simple" tradeoff
 * vmm_free_user_space() already makes for the whole-address-space
 * case, just not walking back up to check emptiness here. Only a
 * local invlpg, not a full vmm_flush_tlb_all_cpus() shootdown: every
 * caller targets a *user* address space belonging to exactly one
 * task, and a task only ever runs on one CPU at a time, so no other
 * core can have this mapping cached. Callers that unmap kernel-shared
 * mappings (there are none yet) would need the cross-cpu variant
 * instead. */
void vmm_unmap_page_in(uint64_t pml4_phys, uint64_t virt);
void vmm_unmap_range_in(uint64_t pml4_phys, uint64_t virt, uint64_t size);

/* Frees every page-table level and every mapped physical page in the
 * *lower* half (user space, PML4 indices 0-255) of the given address
 * space, then frees the PML4 itself. Never touches the shared upper
 * half. Call once a process has exited and nothing references its
 * address space any more (in particular: not the currently loaded
 * CR3 on any CPU). */
void vmm_free_user_space(uint64_t pml4_phys);

/* Maps `size` bytes of device MMIO starting at physical address `phys`
 * as uncacheable, into a dedicated virtual window well above the direct
 * map, and returns a usable pointer (already offset to `phys`'s
 * alignment within the page). Never unmapped -- fine for the handful of
 * fixed platform devices (I/O APIC, xAPIC fallback, ...) this exists
 * for. */
void *vmm_map_mmio(uint64_t phys, size_t size);

/* Invalidates `virt` (or, if `virt` is 0, the whole TLB) on every online
 * CPU, synchronously. Call after modifying a live mapping that another
 * core might already have cached in its TLB. */
void vmm_flush_tlb_all_cpus(uint64_t virt);

#endif /* NEXUS_VMM_H */
