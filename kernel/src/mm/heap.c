#include "mm/heap.h"
#include "boot/requests.h"
#include "mm/pmm.h"
#include "panic.h"
#include "sync/spinlock.h"

#define HEAP_ALIGN 16
#define MIN_GROW_PAGES 32

struct block_header {
  size_t size; // usable bytes following this header
  bool free;
  struct block_header *next; // next block in ADDRESS order
  struct block_header *prev; // Enables backward coalescing.
} __attribute__((aligned(HEAP_ALIGN)));

static struct block_header *heap_head = NULL;
static spinlock_t heap_lock = SPINLOCK_INIT;

static uint64_t used_bytes = 0;
static uint64_t capacity_bytes = 0;

static inline size_t round_up(size_t n, size_t a) {
  return (n + (a - 1)) & ~(a - 1);
}

static bool grow_heap(size_t min_size) {
  size_t need = sizeof(struct block_header) + min_size;
  size_t pages = DIV_ROUND_UP(need, PAGE_SIZE);

  if (pages < MIN_GROW_PAGES) {
    pages = MIN_GROW_PAGES;
  }

  uint64_t phys = pmm_alloc_pages(pages);
  if (phys == 0) {
    return false;
  }

  struct block_header *blk = (struct block_header *)phys_to_virt(phys);
  blk->size = pages * PAGE_SIZE - sizeof(struct block_header);
  blk->free = true;
  capacity_bytes += pages * PAGE_SIZE;

  if (heap_head == NULL || blk < heap_head) {
    blk->prev = NULL;
    blk->next = heap_head;
    if (heap_head != NULL) {
      heap_head->prev = blk;
    }
    heap_head = blk;
    return true;
  }

  struct block_header *cur = heap_head;
  while (cur->next != NULL && cur->next < blk) {
    cur = cur->next;
  }
  blk->next = cur->next;
  blk->prev = cur;
  if (cur->next != NULL) {
    cur->next->prev = blk;
  }
  cur->next = blk;
  return true;
}

void heap_init(void) {
  if (!grow_heap(PAGE_SIZE)) {
    panic("heap: failed to allocate the initial heap region");
  }
}

static void split_block(struct block_header *blk, size_t size) {
  size_t remaining = blk->size - size;

  if (remaining <= sizeof(struct block_header) + HEAP_ALIGN) {
    return; /* not worth splitting */
  }

  uint8_t *new_blk_addr = (uint8_t *)blk + sizeof(struct block_header) + size;
  struct block_header *new_blk = (struct block_header *)new_blk_addr;

  new_blk->size = remaining - sizeof(struct block_header);
  new_blk->free = true;
  new_blk->next = blk->next;
  new_blk->prev = blk;
  if (blk->next != NULL) {
    blk->next->prev = new_blk;
  }

  blk->size = size;
  blk->next = new_blk;
}

void *kmalloc(size_t size) {
  if (size == 0) {
    return NULL;
  }

  size = round_up(size, HEAP_ALIGN);
  uint64_t flags = spinlock_acquire_irqsave(&heap_lock);

  for (;;) {
    for (struct block_header *b = heap_head; b != NULL; b = b->next) {
      if (b->free && b->size >= size) {
        split_block(b, size);
        b->free = false;
        used_bytes += b->size;
        spinlock_release_irqrestore(&heap_lock, flags);
        return (void *)((uint8_t *)b + sizeof(struct block_header));
      }
    }

    if (!grow_heap(size)) {
      spinlock_release_irqrestore(&heap_lock, flags);
      return NULL;
    }
  }
}

void *kzalloc(size_t size) {
  void *p = kmalloc(size);
  if (p != NULL) {
    memset(p, 0, size);
  }
  return p;
}

static void coalesce_forward(struct block_header *b) {
  while (b->next != NULL && b->next->free) {
    uint8_t *end_of_b = (uint8_t *)b + sizeof(struct block_header) + b->size;
    if (end_of_b != (uint8_t *)b->next) {
      break; // not physically adjacent -- separate growth region
    }

    struct block_header *dead = b->next;
    b->size += sizeof(struct block_header) + dead->size;
    b->next = dead->next;
    if (b->next != NULL) {
      b->next->prev = b;
    }
  }
}

void kfree(void *ptr) {
  if (ptr == NULL) {
    return;
  }

  struct block_header *b =
      (struct block_header *)((uint8_t *)ptr - sizeof(struct block_header));

  uint64_t flags = spinlock_acquire_irqsave(&heap_lock);
  b->free = true;
  used_bytes -= b->size;
  if (b->prev != NULL && b->prev->free) {
    uint8_t *end_of_prev =
        (uint8_t *)b->prev + sizeof(struct block_header) + b->prev->size;
    if (end_of_prev == (uint8_t *)b) {
      b = b->prev;
    }
  }
  coalesce_forward(b);

  spinlock_release_irqrestore(&heap_lock, flags);
}

uint64_t heap_used_bytes(void) { return used_bytes; }
uint64_t heap_capacity_bytes(void) { return capacity_bytes; }
