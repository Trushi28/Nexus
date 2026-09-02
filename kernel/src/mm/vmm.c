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

  uint64_t new_phys = pmm_alloc_page(); // pmm hands back zeroed frames
  if (new_phys == 0) {
    panic("vmm: out of memory allocating a page table");
  }

  // Intermediate entries are deliberately permissive
  // (present+writable+user);the leaf PTE is what actually enforces
  // read-only/NX/supervisor.
  table[index] = new_phys | VMM_PRESENT | VMM_WRITABLE | VMM_USER;

  return (uint64_t *)phys_to_virt(new_phys);
}

static void map_page_in(uint64_t *pml4v, uint64_t virt, uint64_t phys,
                        uint64_t flags) {
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

void vmm_map_range(uint64_t virt, uint64_t phys, uint64_t size,
                   uint64_t flags) {
  map_range_in(pml4_virt, virt, phys, size, flags);
}

void vmm_map_page_in(uint64_t pml4_phys, uint64_t virt, uint64_t phys,
                     uint64_t flags) {
  map_page_in((uint64_t *)phys_to_virt(pml4_phys), virt, phys, flags);
}

void vmm_map_range_in(uint64_t pml4_phys, uint64_t virt, uint64_t phys,
                      uint64_t size, uint64_t flags) {
  map_range_in((uint64_t *)phys_to_virt(pml4_phys), virt, phys, size, flags);
}

/* Looks up an existing next-level table without creating one. */
static bool table_lookup(uint64_t *table, size_t index, uint64_t **out) {
  if (!(table[index] & VMM_PRESENT)) {
    return false;
  }
  *out = (uint64_t *)phys_to_virt(table[index] & ADDR_MASK);
  return true;
}

static void unmap_page_in(uint64_t *pml4v, uint64_t virt) {
  size_t pml4_i = (virt >> 39) & 0x1FF;
  size_t pdpt_i = (virt >> 30) & 0x1FF;
  size_t pd_i = (virt >> 21) & 0x1FF;
  size_t pt_i = (virt >> 12) & 0x1FF;

  uint64_t f = spinlock_acquire_irqsave(&vmm_lock);

  uint64_t *pdpt, *pd, *pt;
  if (!table_lookup(pml4v, pml4_i, &pdpt) || !table_lookup(pdpt, pdpt_i, &pd) ||
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

  /* Share the kernel's upper-half mappings. User mappings occupy the lower
   * half and remain private to each address space. */
  uint64_t f = spinlock_acquire_irqsave(&vmm_lock);
  for (size_t i = 256; i < ENTRIES_PER_TABLE; i++) {
    table[i] = pml4_virt[i];
  }
  spinlock_release_irqrestore(&vmm_lock, f);

  return phys;
}

void vmm_free_user_space(uint64_t pml4_phys) {
  /* The caller guarantees this address space is inactive on every CPU, so
   * no TLB shootdown is required before freeing it. */
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

uint64_t vmm_copy_address_space(uint64_t src_pml4_phys) {
  uint64_t dst_pml4_phys = vmm_new_address_space();
  if (dst_pml4_phys == 0) {
    return 0;
  }

  uint64_t *src_pml4 = (uint64_t *)phys_to_virt(src_pml4_phys);

  for (size_t i = 0; i < 256; i++) {
    if (!(src_pml4[i] & VMM_PRESENT)) {
      continue;
    }
    uint64_t *src_pdpt = (uint64_t *)phys_to_virt(src_pml4[i] & ADDR_MASK);

    for (size_t j = 0; j < ENTRIES_PER_TABLE; j++) {
      if (!(src_pdpt[j] & VMM_PRESENT)) {
        continue;
      }
      uint64_t *src_pd = (uint64_t *)phys_to_virt(src_pdpt[j] & ADDR_MASK);

      for (size_t k = 0; k < ENTRIES_PER_TABLE; k++) {
        if (!(src_pd[k] & VMM_PRESENT)) {
          continue;
        }
        uint64_t *src_pt = (uint64_t *)phys_to_virt(src_pd[k] & ADDR_MASK);

        for (size_t l = 0; l < ENTRIES_PER_TABLE; l++) {
          uint64_t entry = src_pt[l];
          if (!(entry & VMM_PRESENT)) {
            continue;
          }

          uint64_t src_phys = entry & ADDR_MASK;
          /* Preserve the source leaf PTE flags. */
          uint64_t flags = entry & ~ADDR_MASK;

          uint64_t new_phys = pmm_alloc_page();
          if (new_phys == 0) {
            vmm_free_user_space(dst_pml4_phys);
            return 0;
          }
          memcpy(phys_to_virt(new_phys), phys_to_virt(src_phys), PAGE_SIZE);

          uint64_t va = ((uint64_t)i << 39) | ((uint64_t)j << 30) |
                        ((uint64_t)k << 21) | ((uint64_t)l << 12);
          vmm_map_page_in(dst_pml4_phys, va, new_phys, flags);
        }
      }
    }
  }

  return dst_pml4_phys;
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

  /* Map only physical regions reported by the boot memory map. Mapping every
   * address up to the highest physical address can waste page tables on gaps.
   *
   * NOTE: this loop maps EVERY entry regardless of type (not just
   * LIMINE_MEMMAP_USABLE) -- framebuffer, ACPI reclaim/NVS, reserved,
   * bootloader-reclaimable, all of it -- so that anything the bootloader
   * already handed us a valid HHDM pointer for (fb->address, the RSDP,
   * etc.) is still backed by a real mapping once we switch to our own
   * tables below. `direct_map_highest_end` tracks the highest physical
   * address this loop covers, across ALL of those entries -- deliberately
   * NOT the same thing as pmm_total_bytes() (which only totals USABLE
   * memory, see mm/pmm.c). A framebuffer or other reserved BAR commonly
   * lives well above the top of usable RAM, so using pmm_total_bytes()
   * alone here would undercount how far the direct map actually reaches. */
  uint64_t direct_map_highest_end = 0;

  struct limine_memmap_response *mm = g_boot.memmap;
  for (uint64_t i = 0; i < mm->entry_count; i++) {
    struct limine_memmap_entry *e = mm->entries[i];

    uint64_t base = ALIGN_DOWN(e->base, PAGE_SIZE);
    uint64_t length = ALIGN_UP(e->length + (e->base - base), PAGE_SIZE);

    vmm_map_range(g_boot.hhdm_offset + base, base, length,
                  VMM_WRITABLE | VMM_NX);

    uint64_t end = base + length;
    if (end > direct_map_highest_end) {
      direct_map_highest_end = end;
    }
  }

  /* The kernel image itself, W^X, matching linker.ld's layout. */
  map_kernel_section((uint64_t)__text_start, (uint64_t)__text_end, 0);
  map_kernel_section((uint64_t)__rodata_start, (uint64_t)__rodata_end, VMM_NX);
  map_kernel_section((uint64_t)__data_start, (uint64_t)__bss_end,
                     VMM_WRITABLE | VMM_NX);

  /* Reserve a virtual window for MMIO mappings, starting strictly above
   * every physical address the direct map above already covers (see
   * direct_map_highest_end's own comment) -- NOT above pmm_total_bytes(),
   * which would leave anything mapped between "usable RAM" and here
   * (the framebuffer, reserved firmware regions, ...) exposed to being
   * silently reassigned to a driver's MMIO BAR the moment vmm_map_mmio()
   * hands out a virtual page that collides with it. Both this window and
   * the direct map share the same hhdm_offset+X addressing convention,
   * so this is the one thing standing between "device driver maps its
   * BAR" and "device driver quietly overwrites the framebuffer's PTE" --
   * vmm_map_mmio()'s own collision check below is the second, structural
   * line of defense in case this bound is ever wrong. */
  mmio_next_virt = ALIGN_UP(g_boot.hhdm_offset + direct_map_highest_end,
                            0x40000000ULL /* 1GiB */);

  kprintf(
      "[vmm] direct map covers up to phys 0x%p; MMIO window reserved at 0x%p\n",
      (void *)direct_map_highest_end, (void *)mmio_next_virt);

  register_interrupt_handler(VEC_TLB_SHOOTDOWN_IPI, tlb_shootdown_handler);

  kprintf("[vmm] direct map populated, switching to our own page tables\n");

  write_cr3(pml4_phys_addr);
}

uint64_t vmm_kernel_pml4_phys(void) { return pml4_phys_addr; }

void vmm_load_kernel_pagemap(void) { write_cr3(pml4_phys_addr); }

/* True if every page in [virt, virt+size) is currently unmapped in the
 * kernel's own page tables. vmm_map_mmio()'s only line of defense
 * against silently reassigning a virtual page that's already serving
 * as someone else's mapping (the framebuffer's direct map, most
 * visibly) -- table_get_or_create()/map_page_in() have no such check
 * of their own, since intermediate-table creation legitimately expects
 * to find existing entries there. This walks read-only via
 * table_lookup(), so it never creates a table just to answer the
 * question. */
static bool range_is_unmapped(uint64_t virt, uint64_t size) {
  uint64_t start = ALIGN_DOWN(virt, PAGE_SIZE);
  uint64_t end = ALIGN_UP(virt + size, PAGE_SIZE);

  uint64_t f = spinlock_acquire_irqsave(&vmm_lock);
  for (uint64_t v = start; v < end; v += PAGE_SIZE) {
    size_t pml4_i = (v >> 39) & 0x1FF;
    size_t pdpt_i = (v >> 30) & 0x1FF;
    size_t pd_i = (v >> 21) & 0x1FF;
    size_t pt_i = (v >> 12) & 0x1FF;

    uint64_t *pdpt, *pd, *pt;
    if (!table_lookup(pml4_virt, pml4_i, &pdpt) ||
        !table_lookup(pdpt, pdpt_i, &pd) || !table_lookup(pd, pd_i, &pt)) {
      continue; /* no intermediate table at all -- definitely unmapped */
    }
    if (pt[pt_i] & VMM_PRESENT) {
      spinlock_release_irqrestore(&vmm_lock, f);
      return false;
    }
  }
  spinlock_release_irqrestore(&vmm_lock, f);
  return true;
}

void *vmm_map_mmio(uint64_t phys, size_t size) {
  uint64_t aligned = ALIGN_DOWN(phys, PAGE_SIZE);
  uint64_t page_off = phys - aligned;
  uint64_t map_size = ALIGN_UP(size + page_off, PAGE_SIZE);

  uint64_t f = spinlock_acquire_irqsave(&mmio_lock);
  uint64_t virt = mmio_next_virt;
  mmio_next_virt += map_size;
  spinlock_release_irqrestore(&mmio_lock, f);

  /* Structural guard: this virtual range must not already be mapped to
   * something else (the framebuffer's direct map is the case that
   * actually bit us -- see vmm_init()'s comment on direct_map_highest_end).
   * A collision here means the mmio window computation is wrong for
   * this machine's memory layout; panic loudly instead of silently
   * repointing whatever used to live at this address. */
  if (!range_is_unmapped(virt, map_size)) {
    panic("vmm_map_mmio: virt 0x%p..0x%p for phys 0x%p (size 0x%p) "
          "collides with an existing mapping -- MMIO window is "
          "overlapping the direct map (see vmm_init())",
          (void *)virt, (void *)(virt + map_size), (void *)phys, (void *)size);
  }

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
