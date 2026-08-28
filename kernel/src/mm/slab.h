#ifndef NEXUS_SLAB_H
#define NEXUS_SLAB_H

#include "klib/klib.h"
#include "sync/spinlock.h"

/*
 * Fixed-size object allocator.
 *
 * Each cache manages same-sized slots carved from PMM pages. Free slots
 * store the next free-list pointer in the object itself, so no per-object
 * header is required.
 *
 * Slab pages are retained for the lifetime of the cache.
 */

struct slab_cache {
  size_t obj_size; // rounded up to a multiple of 8
  size_t objs_per_page;
  void *free_list;
  spinlock_t lock;
  const char *name;
  uint64_t num_allocated; // live objects currently checked out
  uint64_t num_pages;     // pages ever committed -- never reclaimed
};

// Initializes a cache for objects of `obj_size` bytes. Allocation is lazy: the first slab_alloc() call commits the first backing page.
void slab_cache_init(struct slab_cache *cache, size_t obj_size, const char *name);

// Returns a zeroed object, or NULL on OOM.
void *slab_alloc(struct slab_cache *cache);

// Returns `ptr` to `cache`. `ptr` must have been allocated from this cache A NULL pointer is ignored.
void slab_free(struct slab_cache *cache, void *ptr);

#endif /* NEXUS_SLAB_H */
