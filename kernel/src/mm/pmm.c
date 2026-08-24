#include "mm/pmm.h"
#include "boot/requests.h"
#include "debug/log.h"
#include "panic.h"
#include "sync/spinlock.h"
#include <limine.h>

static uint64_t *bitmap; /* 1 bit per page; 1 = used, 0 = free */
static uint64_t bitmap_words;
static uint64_t total_pages;
static uint64_t free_pages;
static uint64_t last_alloc_index = 0; /* O(1) start tracking */
static spinlock_t pmm_lock = SPINLOCK_INIT;

static inline void bit_set(uint64_t page) {
  bitmap[page / 64] |= (1ULL << (page % 64));
}

static inline void bit_clear(uint64_t page) {
  bitmap[page / 64] &= ~(1ULL << (page % 64));
}

static inline bool bit_test(uint64_t page) {
  return (bitmap[page / 64] & (1ULL << (page % 64))) != 0;
}

static void mark_range(uint64_t phys_base, uint64_t length, bool used) {
  uint64_t start_page = phys_base / PAGE_SIZE;
  uint64_t end_page = (phys_base + length + PAGE_SIZE - 1) / PAGE_SIZE;
  if (end_page > total_pages) {
    end_page = total_pages;
  }

  for (uint64_t p = start_page; p < end_page; p++) {
    bool was_used = bit_test(p);
    if (used && !was_used) {
      bit_set(p);
      free_pages--;
    } else if (!used && was_used) {
      bit_clear(p);
      free_pages++;
    }
  }
}

void pmm_init(void) {
  struct limine_memmap_response *mm = g_boot.memmap;
  uint64_t highest = 0;

  for (uint64_t i = 0; i < mm->entry_count; i++) {
    struct limine_memmap_entry *e = mm->entries[i];
    if (e->type != LIMINE_MEMMAP_USABLE) {
      continue;
    }
    uint64_t end = e->base + e->length;
    if (end > highest) {
      highest = end;
    }
  }

  total_pages = highest / PAGE_SIZE;
  bitmap_words = DIV_ROUND_UP(total_pages, 64);
  uint64_t bitmap_bytes = ALIGN_UP(bitmap_words * 8, PAGE_SIZE);

  /* Find a big enough usable region to hold the bitmap itself. */
  uint64_t bitmap_phys = 0;
  for (uint64_t i = 0; i < mm->entry_count; i++) {
    struct limine_memmap_entry *e = mm->entries[i];
    if (e->type == LIMINE_MEMMAP_USABLE && e->length >= bitmap_bytes) {
      bitmap_phys = e->base;
      break;
    }
  }

  if (bitmap_phys == 0) {
    panic("pmm: no usable region large enough for the frame bitmap "
          "(%lu bytes needed)",
          bitmap_bytes);
  }

  bitmap = (uint64_t *)phys_to_virt(bitmap_phys);
  memset(bitmap, 0xFF, bitmap_words * 8); /* everything starts "used" */
  free_pages = 0;

  for (uint64_t i = 0; i < mm->entry_count; i++) {
    struct limine_memmap_entry *e = mm->entries[i];
    if (e->type == LIMINE_MEMMAP_USABLE) {
      mark_range(e->base, e->length, false);
    }
  }

  /* Reserve the bitmap's own backing pages. */
  mark_range(bitmap_phys, bitmap_bytes, true);

  /* Reserve page 0 to avoid triggering "NULL" allocation crashes. */
  mark_range(0, PAGE_SIZE, true);

  kprintf("[pmm] %lu MiB total, %lu MiB free, bitmap at 0x%p (%lu KiB)\n",
          (highest) / (1024 * 1024), (free_pages * PAGE_SIZE) / (1024 * 1024),
          (void *)bitmap_phys, bitmap_bytes / 1024);
}

uint64_t pmm_alloc_pages(size_t count) {
  if (count == 0) {
    return 0;
  }

  uint64_t flags = spinlock_acquire_irqsave(&pmm_lock);
  uint64_t run_start = UINT64_MAX;
  uint64_t run_len = 0;
  uint64_t found = UINT64_MAX;

  /* Phase 1: Search from last_alloc_index up to total_pages */
  for (uint64_t p = last_alloc_index; p < total_pages; p++) {
    /* FAST SKIP: Jump over fully-allocated 64-page blocks in O(1) */
    if (run_len == 0 && (p % 64) == 0 && bitmap[p / 64] == 0xFFFFFFFFFFFFFFFF) {
      p += 63;
      continue;
    }
    if (!bit_test(p)) {
      if (run_len == 0)
        run_start = p;
      run_len++;
      if (run_len == count) {
        found = run_start;
        goto done;
      }
    } else {
      run_len = 0;
    }
  }

  /* Phase 2: Wrap around and search from 0 to last_alloc_index */
  run_len = 0;
  for (uint64_t p = 0; p < last_alloc_index; p++) {
    /* FAST SKIP */
    if (run_len == 0 && (p % 64) == 0 && bitmap[p / 64] == 0xFFFFFFFFFFFFFFFF) {
      p += 63;
      continue;
    }
    if (!bit_test(p)) {
      if (run_len == 0)
        run_start = p;
      run_len++;
      if (run_len == count) {
        found = run_start;
        goto done;
      }
    } else {
      run_len = 0;
    }
  }

done:
  if (found == UINT64_MAX) {
    spinlock_release_irqrestore(&pmm_lock, flags);
    return 0;
  }

  for (uint64_t p = found; p < found + count; p++) {
    bit_set(p);
  }

  free_pages -= count;
  last_alloc_index = (found + count) % total_pages;

  spinlock_release_irqrestore(&pmm_lock, flags);

  uint64_t phys = found * PAGE_SIZE;
  memset(phys_to_virt(phys), 0, count * PAGE_SIZE);

  return phys;
}

uint64_t pmm_alloc_page(void) { return pmm_alloc_pages(1); }

void pmm_free_pages(uint64_t phys, size_t count) {
  if (phys == 0) {
    return;
  }

  uint64_t start_page = phys / PAGE_SIZE;
  uint64_t flags = spinlock_acquire_irqsave(&pmm_lock);

  for (uint64_t p = start_page; p < start_page + count; p++) {
    if (bit_test(p)) {
      bit_clear(p);
      free_pages++;
    }
  }

  spinlock_release_irqrestore(&pmm_lock, flags);
}

void pmm_free_page(uint64_t phys) { pmm_free_pages(phys, 1); }

uint64_t pmm_total_bytes(void) { return total_pages * PAGE_SIZE; }
uint64_t pmm_free_bytes(void) { return free_pages * PAGE_SIZE; }
uint64_t pmm_used_bytes(void) { return (total_pages - free_pages) * PAGE_SIZE; }
