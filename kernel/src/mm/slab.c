#include "mm/slab.h"
#include "boot/requests.h"
#include "mm/pmm.h"

void slab_cache_init(struct slab_cache *cache, size_t obj_size, const char *name) {
  cache->obj_size = ALIGN_UP(MAX(obj_size, sizeof(void *)), 8);
  cache->objs_per_page = PAGE_SIZE / cache->obj_size;
  cache->free_list = NULL;
  spinlock_init(&cache->lock);
  cache->name = name;
  cache->num_allocated = 0;
  cache->num_pages = 0;
}

// Caller holds cache->lock. Adds one PMM page and links its slots into the cache free list.
static bool slab_grow_locked(struct slab_cache *cache) {
  uint64_t phys = pmm_alloc_page();
  if (phys == 0) {
    return false;
  }

  uint8_t *page = (uint8_t *)phys_to_virt(phys);
  for (size_t i = 0; i < cache->objs_per_page; i++) {
    void *slot = page + i * cache->obj_size;
    *(void **)slot = cache->free_list;
    cache->free_list = slot;
  }
  cache->num_pages++;
  return true;
}

void *slab_alloc(struct slab_cache *cache) {
  uint64_t f = spinlock_acquire_irqsave(&cache->lock);

  if (cache->free_list == NULL && !slab_grow_locked(cache)) {
    spinlock_release_irqrestore(&cache->lock, f);
    return NULL;
  }

  void *obj = cache->free_list;
  cache->free_list = *(void **)obj;
  cache->num_allocated++;

  spinlock_release_irqrestore(&cache->lock, f);

  // Safe after unlocking: the object is no longer on the free list.
  memset(obj, 0, cache->obj_size);
  return obj;
}

void slab_free(struct slab_cache *cache, void *ptr) {
  if (ptr == NULL) {
    return;
  }
  uint64_t f = spinlock_acquire_irqsave(&cache->lock);
  *(void **)ptr = cache->free_list;
  cache->free_list = ptr;
  cache->num_allocated--;
  spinlock_release_irqrestore(&cache->lock, f);
}
