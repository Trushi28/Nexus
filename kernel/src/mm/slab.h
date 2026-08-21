#ifndef NEXUS_SLAB_H
#define NEXUS_SLAB_H

#include "klib/klib.h"
#include "sync/spinlock.h"

/*
 * A small, explicit-opt-in slab allocator for fixed-size objects that
 * get allocated and freed often -- struct task (every run/split/exit)
 * is the first real user (see sched.c). Deliberately NOT a drop-in
 * replacement for kmalloc()/kfree(): mm/heap.c's general first-fit
 * allocator already handles the variable-size, comparatively rare
 * allocations (graph/tmpfs file buffers, gedges, vfs_files, ...) just
 * fine, and its block_header overhead only actually hurts for SMALL,
 * FREQUENT, FIXED-size objects -- exactly the case this exists to
 * cover instead.
 *
 * Why explicit opt-in rather than a transparent kmalloc() size-class
 * fast path: kfree() only ever gets a bare pointer, so a transparent
 * design would need to figure out at free time whether a given
 * pointer came from a slab or from the general heap -- either via a
 * per-object header (which reintroduces exactly the per-object
 * overhead this exists to avoid) or by sniffing a magic number at the
 * enclosing page boundary (which is a real memory-safety hazard: nothing
 * stops an ordinary heap block's own bytes from coincidentally
 * matching). Making the caller supply the right cache back to
 * slab_free() removes the ambiguity entirely, at the cost of the
 * caller needing to remember which cache an object came from -- a
 * cost every real kmem_cache-style allocator accepts for the same
 * reason.
 *
 * Design: each cache is a singly-linked free list of same-sized slots
 * carved out of whole PMM pages. A free slot's own first 8 bytes ARE
 * the "next free slot" pointer -- there's no separate per-object
 * header at all, so slab_alloc() from a warm cache costs exactly one
 * pointer-chase, and there's zero memory overhead beyond whatever
 * padding ALIGN_UP(obj_size, 8) added.
 *
 * Like mm/heap.c's own kfree(), a freed slot is never handed back to
 * the PMM -- the next slab_alloc() on the same cache will almost
 * certainly want a slot again soon, so there's nothing to gain by
 * shrinking. No page-eviction path exists in v1.
 */

struct slab_cache {
  size_t obj_size;         /* rounded up to a multiple of 8 */
  size_t objs_per_page;
  void *free_list;
  spinlock_t lock;
  const char *name;        /* diagnostics only, e.g. `meminfo` */
  uint64_t num_allocated;  /* live objects currently checked out */
  uint64_t num_pages;      /* pages ever committed -- never reclaimed */
};

/* Prepares `cache` to serve objects of `obj_size` bytes each. Doesn't
 * allocate anything itself -- the first slab_alloc() call grows the
 * cache by one page on demand, same lazy-growth philosophy as
 * mm/heap.c's own grow_heap(). `name` must outlive the cache (a
 * string literal at the call site is normal, same convention
 * vnode_ops tables use for their own static lifetime). Call once,
 * before any slab_alloc()/slab_free() on this cache -- there's no
 * teardown function, since nothing in this kernel ever tears a cache
 * back down while still running. */
void slab_cache_init(struct slab_cache *cache, size_t obj_size, const char *name);

/* Returns a zeroed object from the cache (same convention as
 * kzalloc()), or NULL on OOM. */
void *slab_alloc(struct slab_cache *cache);

/* Returns `ptr` -- which MUST have come from slab_alloc() on this
 * EXACT cache, not any other one and not from kmalloc()/kzalloc() --
 * to the free list. A no-op if `ptr` is NULL. */
void slab_free(struct slab_cache *cache, void *ptr);

#endif /* NEXUS_SLAB_H */
