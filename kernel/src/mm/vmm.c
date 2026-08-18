#include "mm/vmm.h"
#include "apic/lapic.h"
#include "boot/requests.h"
#include "cpu/io.h"
#include "cpu/isr.h"
#include "cpu/vectors.h"
#include "debug/log.h"
#include "mm/pmm.h"
#include "panic.h"
#include "smp/smp.h"
#include "sync/spinlock.h"

extern char __text_start[], __text_end[];
extern char __rodata_start[], __rodata_end[];
extern char __data_start[], __bss_end[];

#define ENTRIES_PER_TABLE 512
#define ADDR_MASK 0x000FFFFFFFFFF000ULL

static uint64_t *pml4_virt;
static uint64_t pml4_phys_addr;
static spinlock_t vmm_lock = SPINLOCK_INIT;

static uint64_t mmio_next_virt;
static spinlock_t mmio_lock = SPINLOCK_INIT;

static void tlb_shootdown_handler(struct interrupt_frame *frame);

static uint64_t *table_get_or_create(uint64_t *table, size_t index) {
  if (table[index] & VMM_PRESENT) {
    return (uint64_t *)phys_to_virt(table[index] & ADDR_MASK);
  }

  uint64_t new_phys = pmm_alloc_page(); /* pmm hands back zeroed frames */
  if (new_phys == 0) {
    panic("vmm: out of memory allocating a page table");
  }

  /* Intermediate entries are deliberately permissive (present+writable+user);
   * the leaf PTE is what actually enforces read-only/NX/supervisor. */
  table[index] = new_phys | VMM_PRESENT | VMM_WRITABLE | VMM_USER;

  return (uint64_t *)phys_to_virt(new_phys);
}

/* Shared worker behind every vmm_map_page*() variant -- `pml4v` is the
 * HHDM-mapped virtual pointer to whichever PML4 (kernel's own, or a
 * process's) is being modified. table_get_or_create() is already
 * pml4-agnostic (it just walks whatever `table` it's handed), so this
 * is the only place that needed to grow a parameter. */
static void map_page_in(uint64_t *pml4v, uint64_t virt, uint64_t phys, uint64_t flags) {
  size_t pml4_i = (virt >> 39) & 0x1FF;
  size_t pdpt_i = (virt >> 30) & 0x1FF;
  size_t pd_i = (virt >> 21) & 0x1FF;
  size_t pt_i = (virt >> 12) & 0x1FF;

  uint64_t f = spinlock_acquire_irqsave(&vmm_lock);

  uint64_t *pdpt = table_get_or_create(pml4v, pml4_i);
  uint64_t *pd = table_get_or_create(pdpt, pdpt_i);
  uint64_t *pt = table_get_or_create(pd, pd_i);

  pt[pt_i] = (phys & ADDR_MASK) | flags | VMM_PRESENT;

  spinlock_release_irqrestore(&vmm_lock, f);
}

static void map_range_in(uint64_t *pml4v, uint64_t virt, uint64_t phys,
                          uint64_t size, uint64_t flags) {
  uint64_t start = ALIGN_DOWN(virt, PAGE_SIZE);
  uint64_t end = ALIGN_UP(virt + size, PAGE_SIZE);
  uint64_t off = 0;

  for (uint64_t v = start; v < end; v += PAGE_SIZE, off += PAGE_SIZE) {
    map_page_in(pml4v, v, phys + off, flags);
  }
}

void vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags) {
  map_page_in(pml4_virt, virt, phys, flags);
}

void vmm_map_range(uint64_t virt, uint64_t phys, uint64_t size, uint64_t flags) {
  map_range_in(pml4_virt, virt, phys, size, flags);
}

void vmm_map_page_in(uint64_t pml4_phys, uint64_t virt, uint64_t phys, uint64_t flags) {
  map_page_in((uint64_t *)phys_to_virt(pml4_phys), virt, phys, flags);
}

void vmm_map_range_in(uint64_t pml4_phys, uint64_t virt, uint64_t phys,
                       uint64_t size, uint64_t flags) {
  map_range_in((uint64_t *)phys_to_virt(pml4_phys), virt, phys, size, flags);
}

/* Walks to the leaf PT for `table[index]`, returning false (without
 * creating anything) if any intermediate level isn't present -- the
 * read-only counterpart to table_get_or_create() above, for the
 * unmap path, which must never populate a table that a real mapping
 * never touched. */
static bool table_lookup(uint64_t *table, size_t index, uint64_t **out) {
  if (!(table[index] & VMM_PRESENT)) {
    return false;
  }
  *out = (uint64_t *)phys_to_virt(table[index] & ADDR_MASK);
  return true;
}

/* Shared worker behind vmm_unmap_page_in()/vmm_unmap_range_in() --
 * see vmm.h's comment on both for the locking/TLB reasoning. */
static void unmap_page_in(uint64_t *pml4v, uint64_t virt) {
  size_t pml4_i = (virt >> 39) & 0x1FF;
  size_t pdpt_i = (virt >> 30) & 0x1FF;
  size_t pd_i = (virt >> 21) & 0x1FF;
  size_t pt_i = (virt >> 12) & 0x1FF;

  uint64_t f = spinlock_acquire_irqsave(&vmm_lock);

  uint64_t *pdpt, *pd, *pt;
  if (!table_lookup(pml4v, pml4_i, &pdpt) ||
      !table_lookup(pdpt, pdpt_i, &pd) ||
      !table_lookup(pd, pd_i, &pt)) {
    spinlock_release_irqrestore(&vmm_lock, f);
    return; /* nothing mapped along this path -- nothing to undo */
  }

  uint64_t entry = pt[pt_i];
  if (!(entry & VMM_PRESENT)) {
    spinlock_release_irqrestore(&vmm_lock, f);
    return;
  }
  pt[pt_i] = 0;

  spinlock_release_irqrestore(&vmm_lock, f);

  invlpg(virt);
  pmm_free_page(entry & ADDR_MASK);
}

void vmm_unmap_page_in(uint64_t pml4_phys, uint64_t virt) {
  unmap_page_in((uint64_t *)phys_to_virt(pml4_phys), virt);
}

void vmm_unmap_range_in(uint64_t pml4_phys, uint64_t virt, uint64_t size) {
  uint64_t start = ALIGN_DOWN(virt, PAGE_SIZE);
  uint64_t end = ALIGN_UP(virt + size, PAGE_SIZE);
  uint64_t *pml4v = (uint64_t *)phys_to_virt(pml4_phys);

  for (uint64_t v = start; v < end; v += PAGE_SIZE) {
    unmap_page_in(pml4v, v);
  }
}

uint64_t vmm_new_address_space(void) {
  uint64_t phys = pmm_alloc_page(); /* zeroed */
  if (phys == 0) {
    return 0;
  }
  uint64_t *table = (uint64_t *)phys_to_virt(phys);

  /* Share the kernel's upper-half mapping (direct map, kernel image,
   * MMIO window -- everything vmm_init() built) by copying its
   * top-level PML4 entries wholesale. Cheap (256 qwords) and correct:
   * each copied entry just points at an existing shared sub-table, so
   * any later change *within* an already-shared subtree (e.g. a new
   * MMIO window carved out of the existing region) is automatically
   * visible here too. The one gap: a brand new top-level PML4 slot
   * (a fresh 512GiB-aligned region) populated by the kernel *after*
   * this address space was created would not be -- doesn't happen for
   * Nexus's fixed set of kernel regions, but worth knowing. */
  uint64_t f = spinlock_acquire_irqsave(&vmm_lock);
  for (size_t i = 256; i < ENTRIES_PER_TABLE; i++) {
    table[i] = pml4_virt[i];
  }
  spinlock_release_irqrestore(&vmm_lock, f);

  return phys;
}

void vmm_free_user_space(uint64_t pml4_phys) {
  /* No TLB shootdown needed here: by the time this runs, the caller
   * has guaranteed the address space isn't the currently loaded CR3 on
   * any CPU (see the callers), and without PCID (never enabled here),
   * every CR3 write already flushes the whole TLB -- so whatever
   * stale entries this address space may have left behind were wiped
   * out the moment any CPU switched away from it. */
  uint64_t *pml4 = (uint64_t *)phys_to_virt(pml4_phys);

  for (size_t i = 0; i < 256; i++) {
    if (!(pml4[i] & VMM_PRESENT)) {
      continue;
    }
    uint64_t *pdpt = (uint64_t *)phys_to_virt(pml4[i] & ADDR_MASK);

    for (size_t j = 0; j < ENTRIES_PER_TABLE; j++) {
      if (!(pdpt[j] & VMM_PRESENT)) {
        continue;
      }
      uint64_t *pd = (uint64_t *)phys_to_virt(pdpt[j] & ADDR_MASK);

      for (size_t k = 0; k < ENTRIES_PER_TABLE; k++) {
        if (!(pd[k] & VMM_PRESENT)) {
          continue;
        }
        uint64_t *pt = (uint64_t *)phys_to_virt(pd[k] & ADDR_MASK);

        for (size_t l = 0; l < ENTRIES_PER_TABLE; l++) {
          if (pt[l] & VMM_PRESENT) {
            pmm_free_page(pt[l] & ADDR_MASK);
          }
        }
        pmm_free_page(pd[k] & ADDR_MASK); /* the PT itself */
      }
      pmm_free_page(pdpt[j] & ADDR_MASK); /* the PD itself */
    }
    pmm_free_page(pml4[i] & ADDR_MASK); /* the PDPT itself */
  }

  pmm_free_page(pml4_phys);
}

static void map_kernel_section(uint64_t start, uint64_t end, uint64_t flags) {
  uint64_t s = ALIGN_DOWN(start, PAGE_SIZE);
  uint64_t e = ALIGN_UP(end, PAGE_SIZE);
  uint64_t phys = g_boot.exec_phys_base + (s - g_boot.exec_virt_base);
  vmm_map_range(s, phys, e - s, flags);
}

void vmm_init(void) {
  pml4_phys_addr = pmm_alloc_page();
  if (pml4_phys_addr == 0) {
    panic("vmm: could not allocate the kernel PML4");
  }
  pml4_virt = (uint64_t *)phys_to_virt(pml4_phys_addr);

  /* Direct map: only map the specific physical regions reported by Limine.
   * Blindly mapping 0 to highest exhausts memory by allocating page tables
   * for massive void gaps (e.g. 1 TiB QEMU boundaries). */
  struct limine_memmap_response *mm = g_boot.memmap;
  for (uint64_t i = 0; i < mm->entry_count; i++) {
    struct limine_memmap_entry *e = mm->entries[i];

    uint64_t base = ALIGN_DOWN(e->base, PAGE_SIZE);
    uint64_t length = ALIGN_UP(e->length + (e->base - base), PAGE_SIZE);

    vmm_map_range(g_boot.hhdm_offset + base, base, length,
                  VMM_WRITABLE | VMM_NX);
  }

  /* The kernel image itself, W^X, matching linker.ld's layout. */
  map_kernel_section((uint64_t)__text_start, (uint64_t)__text_end, 0);
  map_kernel_section((uint64_t)__rodata_start, (uint64_t)__rodata_end, VMM_NX);
  map_kernel_section((uint64_t)__data_start, (uint64_t)__bss_end,
                     VMM_WRITABLE | VMM_NX);

  /* Reserve a virtual window for MMIO mappings */
  mmio_next_virt = ALIGN_UP(g_boot.hhdm_offset + pmm_total_bytes(),
                            0x40000000ULL /* 1GiB */);

  register_interrupt_handler(VEC_TLB_SHOOTDOWN_IPI, tlb_shootdown_handler);

  kprintf("[vmm] direct map populated, switching to our own page tables\n");

  write_cr3(pml4_phys_addr);
}

uint64_t vmm_kernel_pml4_phys(void) { return pml4_phys_addr; }

void vmm_load_kernel_pagemap(void) { write_cr3(pml4_phys_addr); }

void *vmm_map_mmio(uint64_t phys, size_t size) {
  uint64_t aligned = ALIGN_DOWN(phys, PAGE_SIZE);
  uint64_t page_off = phys - aligned;
  uint64_t map_size = ALIGN_UP(size + page_off, PAGE_SIZE);

  uint64_t f = spinlock_acquire_irqsave(&mmio_lock);
  uint64_t virt = mmio_next_virt;
  mmio_next_virt += map_size;
  spinlock_release_irqrestore(&mmio_lock, f);

  /* PWT+PCD with no PAT bit selects the "UC" (strong uncacheable) PAT slot */
  vmm_map_range(virt, aligned, map_size,
                VMM_WRITABLE | VMM_NX | VMM_PWT | VMM_PCD);
  return (void *)(virt + page_off);
}

/* --------------------------- TLB shootdown --------------------------- */
static spinlock_t shootdown_lock = SPINLOCK_INIT;
static volatile uint64_t shootdown_addr;
static volatile bool shootdown_full;
static volatile uint32_t shootdown_remaining;

static void tlb_shootdown_handler(struct interrupt_frame *frame) {
  (void)frame;
  if (shootdown_full) {
    write_cr3(read_cr3());
  } else {
    invlpg(shootdown_addr);
  }
  __atomic_fetch_sub(&shootdown_remaining, 1, __ATOMIC_RELEASE);
}

void vmm_flush_tlb_all_cpus(uint64_t virt) {
  if (virt != 0) {
    invlpg(virt);
  } else {
    write_cr3(read_cr3());
  }

  uint32_t others = smp_online_cpu_count();
  if (others <= 1) {
    return;
  }
  others -= 1;

  uint64_t f = spinlock_acquire_irqsave(&shootdown_lock);
  shootdown_addr = virt;
  shootdown_full = (virt == 0);
  __atomic_store_n(&shootdown_remaining, others, __ATOMIC_RELEASE);

  lapic_send_ipi_all_excluding_self(VEC_TLB_SHOOTDOWN_IPI);

  while (__atomic_load_n(&shootdown_remaining, __ATOMIC_ACQUIRE) > 0) {
    asm volatile("pause");
  }
  spinlock_release_irqrestore(&shootdown_lock, f);
}
