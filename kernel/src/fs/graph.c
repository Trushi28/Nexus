#include "fs/graph.h"
#include "boot/requests.h"
#include "debug/log.h"
#include "drivers/blockdev.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "mm/slab.h"
#include "sync/spinlock.h"

static struct slab_cache gnode_cache;
static struct slab_cache gedge_cache;

static spinlock_t graph_lock = SPINLOCK_INIT;

static uint64_t next_node_id = 1;
static struct gnode *registry_head;

struct sstring_entry {
  bool used;
  char name[SSTRING_NAME_MAX];
  struct gnode *anchor;
};
static struct sstring_entry sstrings[SSTRING_MAX_ENTRIES];
/* ---------------------------- dirty tracking ---------------------------
 * Plain atomics, not graph_lock -- a single bool needs cross-cpu
 * visibility, not mutual exclusion with the node/edge data it's
 * tracking. Set unconditionally by every mutating entry point below;
 * cleared by a successful save (graph_save_to_disk() clears it BEFORE
 * taking the snapshot, not after -- see that function for why) or a
 * successful load (a freshly loaded graph matches disk by
 * definition, even though the internal graph_link()/sstring_set()/
 * graph_write() calls deserialize_graph() makes along the way each
 * set the flag individually). */
static volatile bool graph_dirty = false;

static void graph_mark_dirty(void) {
  __atomic_store_n(&graph_dirty, true, __ATOMIC_RELEASE);
}

static void graph_clear_dirty(void) {
  __atomic_store_n(&graph_dirty, false, __ATOMIC_RELEASE);
}

bool graph_is_dirty(void) {
  return __atomic_load_n(&graph_dirty, __ATOMIC_ACQUIRE);
}

void graph_init(void) {
  slab_cache_init(&gnode_cache, sizeof(struct gnode), "gnode");
  slab_cache_init(&gedge_cache, sizeof(struct gedge), "gedge");
  for (uint32_t i = 0; i < SSTRING_MAX_ENTRIES; i++) {
    sstrings[i].used = false;
  }
}

struct gnode *graph_node_create(const char *label) {
  struct gnode *n = slab_alloc(&gnode_cache);
  if (n == NULL) {
    return NULL;
  }
  if (label != NULL) {
    strncpy(n->label, label, sizeof(n->label) - 1);
  }

  uint64_t f = spinlock_acquire_irqsave(&graph_lock);
  n->id = next_node_id++;
  n->reg_next = registry_head;
  registry_head = n;
  spinlock_release_irqrestore(&graph_lock, f);
  graph_mark_dirty();
  return n;
}

static struct gedge *find_edge_locked(struct gnode *from, const char *name) {
  for (struct gedge *e = from->edges; e != NULL; e = e->next) {
    if (strcmp(e->name, name) == 0) {
      return e;
    }
  }
  return NULL;
}

/* Caller holds graph_lock. Frees `start` (already known to have
 * refcount == 0) and cascades: for each of its outgoing edges,
 * decrements that target's refcount and, if it now hits 0 too, queues
 * the same treatment for it. Iterative via an explicit worklist
 * (struct gnode::release_next) rather than recursive -- see that
 * field's comment in graph.h for why. Returns the total number of
 * nodes actually freed (>= 1).
 *
 * Never collects a reference cycle: a node inside one keeps a nonzero
 * refcount forever via the very edges that make it a cycle, so it
 * never gets pushed onto the worklist to begin with. See the file
 * header comment in graph.h -- this is a deliberate, permanent v1
 * boundary, not a bug.
 *
 * Registry removal is a linear scan per freed node -- O(n) per node,
 * so O(n^2) for a large cascade. Fine for the graph sizes this is
 * built for (interactive shell use, capped snapshot size); worth a
 * doubly-linked registry if that ever stops being true. */
static uint32_t release_cascade_locked(struct gnode *start) {
  uint32_t freed = 0;
  struct gnode *worklist = start;
  start->release_next = NULL;

  while (worklist != NULL) {
    struct gnode *n = worklist;
    worklist = n->release_next;

    if (n->refcount != 0) {
      continue; /* re-referenced by something since being queued */
    }

    struct gnode **pp = &registry_head;
    while (*pp != NULL && *pp != n) {
      pp = &(*pp)->reg_next;
    }
    if (*pp == n) {
      *pp = n->reg_next;
    }

    /* Defensive only: a correctly-maintained refcount makes this
     * unreachable (an active sstring anchor always holds a
     * reference, so its target can never legitimately hit 0 here)
     * -- but a dangling sstring pointing at freed memory is a far
     * worse failure than one extra bounded loop, so scrub anyway
     * rather than trust the invariant to never be violated by a
     * future bug elsewhere. */
    for (uint32_t i = 0; i < SSTRING_MAX_ENTRIES; i++) {
      if (sstrings[i].used && sstrings[i].anchor == n) {
        sstrings[i].used = false;
      }
    }

    struct gedge *e = n->edges;
    while (e != NULL) {
      struct gedge *next_e = e->next;
      struct gnode *target = e->target;
      target->refcount--;
      if (target->refcount == 0) {
        target->release_next = worklist;
        worklist = target;
      }
      slab_free(&gedge_cache, e);
      e = next_e;
    }

    if (n->data != NULL) {
      kfree(n->data);
    }
    slab_free(&gnode_cache, e);
    freed++;
  }
  return freed;
}

/* Caller holds graph_lock. Mark-and-sweep over the CURRENT anchor set:
 * BFS-marks everything reachable from every sstring anchor, then frees
 * every registered node the BFS never reached. Shared by
 * graph_collect_cycles() (the standalone `ggc` command -- collects
 * against whatever anchors currently exist) and graph_clear_all()
 * (which has already dropped every anchor by the time it calls this,
 * so here "unreachable" correctly means "everything" -- see that
 * function's own comment). Returns the number of nodes freed. */
static uint32_t collect_cycles_locked(void) {
  for (struct gnode *n = registry_head; n != NULL; n = n->reg_next) {
    n->gc_marked = false;
  }

  struct gnode *worklist = NULL;
  for (uint32_t i = 0; i < SSTRING_MAX_ENTRIES; i++) {
    if (!sstrings[i].used) {
      continue;
    }
    struct gnode *root = sstrings[i].anchor;
    if (!root->gc_marked) {
      root->gc_marked = true;
      root->release_next = worklist;
      worklist = root;
    }
  }

  /* A node with an open classic-VFS fd is a root too, exactly like an
   * sstring anchor -- ordinary refcounting (graph_node_retain(), via
   * grm/gunlink/sstringrm's "refcount==0" checks) already stops those
   * three from freeing a held-open node, but THIS scan doesn't
   * consult refcount at all -- it rebuilds reachability from scratch
   * every time, so without this a node reachable only through an open
   * fd (no anchor, no incoming edge) would look exactly like an
   * unreachable cycle and get swept anyway, leaving that fd dangling. */
  for (struct gnode *n = registry_head; n != NULL; n = n->reg_next) {
    if (n->vfs_open_count > 0 && !n->gc_marked) {
      n->gc_marked = true;
      n->release_next = worklist;
      worklist = n;
    }
  }

  while (worklist != NULL) {
    struct gnode *n = worklist;
    worklist = n->release_next;

    for (struct gedge *e = n->edges; e != NULL; e = e->next) {
      struct gnode *t = e->target;
      if (!t->gc_marked) {
        t->gc_marked = true;
        t->release_next = worklist;
        worklist = t;
      }
    }
  }

  /* Snapshot every unmarked node into its own list BEFORE freeing any
   * of them -- freeing mutates registry_head/reg_next out from under
   * a live iterator over that same list, same reasoning as
   * graph_clear_all()'s old orphan pass used. */
  struct gnode *dead = NULL;
  uint32_t dead_count = 0;
  for (struct gnode *n = registry_head; n != NULL; n = n->reg_next) {
    if (!n->gc_marked) {
      n->release_next = dead;
      dead = n;
      dead_count++;
    }
  }
  if (dead_count == 0) {
    return 0;
  }

  for (struct gnode *n = dead; n != NULL; n = n->release_next) {
    struct gnode **pp = &registry_head;
    while (*pp != NULL && *pp != n) {
      pp = &(*pp)->reg_next;
    }
    if (*pp == n) {
      *pp = n->reg_next;
    }
  }

  /* Free every dead node's own storage. An edge target that's ALSO
   * unmarked (another member of the same cycle/dead group) is left
   * alone -- it's already slated for the same fate elsewhere in this
   * same `dead` list, so decrementing its refcount would be
   * meaningless bookkeeping for a value nobody will read again, and
   * could otherwise trigger a redundant release_cascade_locked() on a
   * node this very loop is about to free directly anyway. An edge
   * target that IS marked (a live, externally-reachable node this
   * dead group merely happened to point at) gets a real decrement --
   * and if that's genuinely its last reference, it goes through the
   * ordinary release_cascade_locked() path, exactly as if a live
   * gunlink had just severed the edge. That decrement can never
   * prematurely free a live target out from under a second dead-group
   * edge pointing at the same node: a target only stays "marked" in
   * the first place because SOME live path (an anchor or a live
   * node's edge) reaches it independent of anything in `dead`, so its
   * refcount can never be reduced to 0 by dead-group edges alone. */
  uint32_t freed = 0;
  for (struct gnode *n = dead; n != NULL;) {
    struct gnode *next_dead = n->release_next;

    struct gedge *e = n->edges;
    while (e != NULL) {
      struct gedge *next_e = e->next;
      struct gnode *target = e->target;
      if (target->gc_marked) {
        target->refcount--;
        if (target->refcount == 0) {
          freed += release_cascade_locked(target);
        }
      }
      slab_free(&gedge_cache, e);
      e = next_e;
    }

    if (n->data != NULL) {
      kfree(n->data);
    }
    slab_free(&gnode_cache, e);
    freed++;

    n = next_dead;
  }

  return freed;
}

uint32_t graph_collect_cycles(void) {
  uint64_t f = spinlock_acquire_irqsave(&graph_lock);
  uint32_t freed = collect_cycles_locked();
  spinlock_release_irqrestore(&graph_lock, f);
  if (freed > 0) {
    graph_mark_dirty();
  }
  return freed;
}

void graph_link(struct gnode *from, const char *edge_name, struct gnode *to) {
  uint64_t f = spinlock_acquire_irqsave(&graph_lock);
  struct gedge *existing = find_edge_locked(from, edge_name);
  if (existing != NULL) {
    struct gnode *old_target = existing->target;
    existing->target = to;
    /* Increment the NEW target before decrementing the old one --
     * not the other way around. If `to` and `old_target` happen to
     * be the same node (repointing an edge at what it already
     * pointed to), decrementing first would transiently touch 0
     * and could free a node this same line is about to
     * re-reference. See this turn's design note for the full
     * reasoning. */
    to->refcount++;
    old_target->refcount--;
    if (old_target->refcount == 0) {
      release_cascade_locked(old_target);
    }
    spinlock_release_irqrestore(&graph_lock, f);
    graph_mark_dirty();
    return;
  }
  spinlock_release_irqrestore(&graph_lock, f);

  struct gedge *e = slab_alloc(&gedge_cache);
  if (e == NULL) {
    return;
  }
  strncpy(e->name, edge_name, sizeof(e->name) - 1);
  e->target = to;

  f = spinlock_acquire_irqsave(&graph_lock);
  existing = find_edge_locked(from, edge_name);
  if (existing != NULL) {
    struct gnode *old_target = existing->target;
    existing->target = to;
    to->refcount++;
    old_target->refcount--;
    if (old_target->refcount == 0) {
      release_cascade_locked(old_target);
    }
    spinlock_release_irqrestore(&graph_lock, f);
    slab_free(&gedge_cache, e);
    graph_mark_dirty();
    return;
  }

  e->next = from->edges;
  from->edges = e;
  from->edge_count++;
  to->refcount++;
  spinlock_release_irqrestore(&graph_lock, f);
  graph_mark_dirty();
}

void graph_unlink(struct gnode *from, const char *edge_name) {
  uint64_t f = spinlock_acquire_irqsave(&graph_lock);

  struct gedge **pp = &from->edges;
  while (*pp != NULL && strcmp((*pp)->name, edge_name) != 0) {
    pp = &(*pp)->next;
  }
  struct gedge *dead = *pp;
  if (dead == NULL) {
    spinlock_release_irqrestore(&graph_lock, f);
    kprintf("[graph] no edge '%s' to remove\n", edge_name);
    return;
  }
  *pp = dead->next;
  from->edge_count--;

  struct gnode *target = dead->target;
  slab_free(&gedge_cache, dead);

  target->refcount--;
  uint32_t freed = 0;
  if (target->refcount == 0) {
    freed = release_cascade_locked(target);
  }

  spinlock_release_irqrestore(&graph_lock, f);
  graph_mark_dirty();
  if (freed > 0) {
    kprintf("[graph] unlinked '%s' -- freed %u node(s)\n", edge_name, freed);
  } else {
    kprintf("[graph] unlinked '%s'\n", edge_name);
  }
}

void graph_node_delete(struct gnode *n) {
  uint64_t f = spinlock_acquire_irqsave(&graph_lock);
  if (n->refcount != 0) {
    uint32_t refs = n->refcount;
    uint64_t id = n->id;
    spinlock_release_irqrestore(&graph_lock, f);
    kprintf("[graph] #%lu is still referenced (refcount=%u) -- unlink it "
            "from everywhere it's used first (gunlink/sstringrm)\n",
            id, refs);
    return;
  }
  uint32_t freed = release_cascade_locked(n);
  spinlock_release_irqrestore(&graph_lock, f);
  graph_mark_dirty();
  kprintf("[graph] freed %u node(s)\n", freed);
}

void graph_clear_all(void) {
  uint64_t f = spinlock_acquire_irqsave(&graph_lock);
  for (uint32_t i = 0; i < SSTRING_MAX_ENTRIES; i++) {
    sstrings[i].used = false;
  }

  uint32_t freed = collect_cycles_locked();

  uint32_t remaining = 0;
  for (struct gnode *n = registry_head; n != NULL; n = n->reg_next) {
    remaining++;
  }

  spinlock_release_irqrestore(&graph_lock, f);

  if (freed > 0) {
    graph_mark_dirty();
  }

  if (remaining == 0) {
    kprintf("[graph] cleared -- freed %u node(s)\n", freed);
  } else {
    kprintf("[graph] cleared -- freed %u node(s), %u node(s) still held "
            "open by a classic-VFS fd (see fs/graphfs_vfs.c) survive and "
            "will free once closed -- if nothing has anything open right "
            "now, that instead points at a bug in collect_cycles_locked(), "
            "please report it\n",
            freed, remaining);
  }
}

struct gnode *graph_edge_lookup(struct gnode *from, const char *name) {
  uint64_t f = spinlock_acquire_irqsave(&graph_lock);
  struct gedge *e = find_edge_locked(from, name);
  struct gnode *target = (e != NULL) ? e->target : NULL;
  spinlock_release_irqrestore(&graph_lock, f);
  return target;
}

bool graph_list_edges(struct gnode *from, uint32_t index, char *name_out,
                      size_t name_max) {
  uint64_t f = spinlock_acquire_irqsave(&graph_lock);
  uint32_t i = 0;
  bool found = false;
  for (struct gedge *e = from->edges; e != NULL; e = e->next, i++) {
    if (i == index) {
      strncpy(name_out, e->name, name_max - 1);
      name_out[name_max - 1] = '\0';
      found = true;
      break;
    }
  }
  spinlock_release_irqrestore(&graph_lock, f);
  return found;
}

static bool ensure_capacity_locked(struct gnode *n, size_t needed) {
  if (needed <= n->capacity) {
    return true;
  }
  size_t new_cap = n->capacity ? n->capacity * 2 : 4096;
  while (new_cap < needed) {
    new_cap *= 2;
  }
  uint8_t *nd = kmalloc(new_cap);
  if (nd == NULL) {
    return false;
  }
  if (n->data != NULL) {
    memcpy(nd, n->data, n->size);
    kfree(n->data);
  }
  n->data = nd;
  n->capacity = new_cap;
  return true;
}

size_t graph_read(struct gnode *n, uint64_t offset, void *buf, size_t len) {
  uint64_t f = spinlock_acquire_irqsave(&graph_lock);
  if (offset >= n->size) {
    spinlock_release_irqrestore(&graph_lock, f);
    return 0;
  }
  size_t avail = (size_t)(n->size - offset);
  size_t n_read = MIN(len, avail);
  memcpy(buf, n->data + offset, n_read);
  spinlock_release_irqrestore(&graph_lock, f);
  return n_read;
}

size_t graph_write(struct gnode *n, uint64_t offset, const void *buf,
                   size_t len) {
  uint64_t f = spinlock_acquire_irqsave(&graph_lock);
  uint64_t end = offset + len;

  if (!ensure_capacity_locked(n, (size_t)end)) {
    spinlock_release_irqrestore(&graph_lock, f);
    return 0;
  }
  if (offset > n->size) {
    memset(n->data + n->size, 0, (size_t)(offset - n->size));
  }
  memcpy(n->data + offset, buf, len);
  if (end > n->size) {
    n->size = end;
  }
  spinlock_release_irqrestore(&graph_lock, f);
  graph_mark_dirty();
  return len;
}

void graph_truncate(struct gnode *n) {
  uint64_t f = spinlock_acquire_irqsave(&graph_lock);
  n->size = 0;
  spinlock_release_irqrestore(&graph_lock, f);
  graph_mark_dirty();
}

void graph_node_snapshot(struct gnode *n, uint64_t *size_out,
                         uint32_t *edge_count_out) {
  uint64_t f = spinlock_acquire_irqsave(&graph_lock);
  if (size_out != NULL) {
    *size_out = n->size;
  }
  if (edge_count_out != NULL) {
    *edge_count_out = n->edge_count;
  }
  spinlock_release_irqrestore(&graph_lock, f);
}

void graph_node_retain(struct gnode *n) {
  uint64_t f = spinlock_acquire_irqsave(&graph_lock);
  n->refcount++;
  n->vfs_open_count++;
  spinlock_release_irqrestore(&graph_lock, f);
}

void graph_node_release(struct gnode *n) {
  uint64_t f = spinlock_acquire_irqsave(&graph_lock);
  n->vfs_open_count--;
  n->refcount--;
  uint32_t freed = 0;
  if (n->refcount == 0) {
    freed = release_cascade_locked(n);
  }
  spinlock_release_irqrestore(&graph_lock, f);
  if (freed > 0) {
    graph_mark_dirty();
  }
}

struct gnode *graph_find_by_id(uint64_t id) {
  uint64_t f = spinlock_acquire_irqsave(&graph_lock);
  struct gnode *result = NULL;
  for (struct gnode *n = registry_head; n != NULL; n = n->reg_next) {
    if (n->id == id) {
      result = n;
      break;
    }
  }
  spinlock_release_irqrestore(&graph_lock, f);
  return result;
}

void graph_for_each_node(void (*fn)(struct gnode *n, void *arg), void *arg) {
  uint64_t f = spinlock_acquire_irqsave(&graph_lock);
  for (struct gnode *n = registry_head; n != NULL; n = n->reg_next) {
    fn(n, arg);
  }
  spinlock_release_irqrestore(&graph_lock, f);
}

bool sstring_set(const char *name, struct gnode *anchor) {
  uint64_t f = spinlock_acquire_irqsave(&graph_lock);

  for (uint32_t i = 0; i < SSTRING_MAX_ENTRIES; i++) {
    if (sstrings[i].used && strcmp(sstrings[i].name, name) == 0) {
      struct gnode *old_anchor = sstrings[i].anchor;
      sstrings[i].anchor = anchor;
      /* Same increment-before-decrement ordering as graph_link()'s
       * repoint path, and for the identical reason. */
      anchor->refcount++;
      old_anchor->refcount--;
      if (old_anchor->refcount == 0) {
        release_cascade_locked(old_anchor);
      }
      spinlock_release_irqrestore(&graph_lock, f);
      graph_mark_dirty();
      return true;
    }
  }
  for (uint32_t i = 0; i < SSTRING_MAX_ENTRIES; i++) {
    if (!sstrings[i].used) {
      sstrings[i].used = true;
      strncpy(sstrings[i].name, name, sizeof(sstrings[i].name) - 1);
      sstrings[i].anchor = anchor;
      anchor->refcount++;
      spinlock_release_irqrestore(&graph_lock, f);
      graph_mark_dirty();
      return true;
    }
  }

  spinlock_release_irqrestore(&graph_lock, f);
  return false;
}

struct gnode *sstring_get(const char *name) {
  uint64_t f = spinlock_acquire_irqsave(&graph_lock);
  struct gnode *result = NULL;
  for (uint32_t i = 0; i < SSTRING_MAX_ENTRIES; i++) {
    if (sstrings[i].used && strcmp(sstrings[i].name, name) == 0) {
      result = sstrings[i].anchor;
      break;
    }
  }
  spinlock_release_irqrestore(&graph_lock, f);
  return result;
}

bool sstring_list(uint32_t index, char *name_out, size_t name_max) {
  uint64_t f = spinlock_acquire_irqsave(&graph_lock);
  uint32_t seen = 0;
  bool found = false;
  for (uint32_t i = 0; i < SSTRING_MAX_ENTRIES; i++) {
    if (!sstrings[i].used) {
      continue;
    }
    if (seen == index) {
      strncpy(name_out, sstrings[i].name, name_max - 1);
      name_out[name_max - 1] = '\0';
      found = true;
      break;
    }
    seen++;
  }
  spinlock_release_irqrestore(&graph_lock, f);
  return found;
}

void sstring_unset(const char *name) {
  uint64_t f = spinlock_acquire_irqsave(&graph_lock);
  for (uint32_t i = 0; i < SSTRING_MAX_ENTRIES; i++) {
    if (sstrings[i].used && strcmp(sstrings[i].name, name) == 0) {
      sstrings[i].used = false;
      struct gnode *anchor = sstrings[i].anchor;
      anchor->refcount--;
      uint32_t freed = 0;
      if (anchor->refcount == 0) {
        freed = release_cascade_locked(anchor);
      }
      spinlock_release_irqrestore(&graph_lock, f);
      graph_mark_dirty();
      if (freed > 0) {
        kprintf("[graph] removed sstring '%s' -- freed %u node(s)\n", name,
                freed);
      } else {
        kprintf("[graph] removed sstring '%s'\n", name);
      }
      return;
    }
  }
  spinlock_release_irqrestore(&graph_lock, f);
  kprintf("[graph] no sstring anchor named '%s'\n", name);
}

struct gnode *graph_resolve(const char *path) {
  char comps[GRAPH_MAX_DEPTH][GEDGE_NAME_MAX];
  int depth = 0;

  const char *p = path;
  while (*p == '/') {
    p++;
  }
  while (*p != '\0' && depth < GRAPH_MAX_DEPTH) {
    size_t n = 0;
    while (*p != '\0' && *p != '/') {
      if (n + 1 < sizeof(comps[0])) {
        comps[depth][n++] = *p;
      }
      p++;
    }
    comps[depth][n] = '\0';
    depth++;
    while (*p == '/') {
      p++;
    }
  }

  if (depth == 0) {
    return NULL;
  }

  struct gnode *cur = sstring_get(comps[0]);
  if (cur == NULL) {
    return NULL;
  }
  for (int i = 1; i < depth; i++) {
    cur = graph_edge_lookup(cur, comps[i]);
    if (cur == NULL) {
      return NULL;
    }
  }
  return cur;
}

struct gnode *graph_touch(const char *path, bool *out_created) {
  char comps[GRAPH_MAX_DEPTH][GEDGE_NAME_MAX];
  int depth = 0;

  const char *p = path;
  while (*p == '/') {
    p++;
  }
  while (*p != '\0' && depth < GRAPH_MAX_DEPTH) {
    size_t n = 0;
    while (*p != '\0' && *p != '/') {
      if (n + 1 < sizeof(comps[0])) {
        comps[depth][n++] = *p;
      }
      p++;
    }
    comps[depth][n] = '\0';
    depth++;
    while (*p == '/') {
      p++;
    }
  }

  if (out_created != NULL) {
    *out_created = false;
  }
  if (depth == 0) {
    return NULL;
  }

  bool created_any = false;

  struct gnode *cur = sstring_get(comps[0]);
  if (cur == NULL) {
    cur = graph_node_create(comps[0]);
    if (cur == NULL) {
      return NULL;
    }
    if (!sstring_set(comps[0], cur)) {
      /* Table full -- the node exists but has no anchor pointing at
       * it, same situation as a bare gmk nobody ever linked
       * anywhere. Not worse than before this function existed; just
       * not the happy path, and worth surfacing since it's
       * surprising for what's supposed to be a one-shot
       * create-and-reach-it call. */
      kprintf("[graph] gtouch: sstring table full -- '%s' created (#%lu) "
              "but not anchored\n",
              comps[0], cur->id);
    }
    created_any = true;
  }

  for (int i = 1; i < depth; i++) {
    struct gnode *next = graph_edge_lookup(cur, comps[i]);
    if (next == NULL) {
      next = graph_node_create(comps[i]);
      if (next == NULL) {
        if (out_created != NULL) {
          *out_created = created_any;
        }
        return NULL;
      }
      graph_link(cur, comps[i], next);
      created_any = true;
    }
    cur = next;
  }

  if (out_created != NULL) {
    *out_created = created_any;
  }
  return cur;
}

/* ============================== persistence ============================== */

#define GRAPH_SNAPSHOT_MAX_BYTES (1u * 1024 * 1024)

#define GRAPH_DISK_VERSION 1
#define GRAPH_DISK_LBA_SUPERBLOCK 0
#define GRAPH_DISK_LBA_PAYLOAD 1

struct PACKED graph_disk_superblock {
  char magic[8];
  uint32_t version;
  uint32_t checksum;
  uint64_t payload_bytes;
  uint64_t next_node_id;
};
_Static_assert(sizeof(struct graph_disk_superblock) <= 512,
               "must fit inside the smallest realistic NVMe sector size");

static uint32_t fnv1a32(const void *data, size_t len) {
  const uint8_t *p = (const uint8_t *)data;
  uint32_t h = 0x811C9DC5u;
  for (size_t i = 0; i < len; i++) {
    h ^= p[i];
    h *= 0x01000193u;
  }
  return h;
}

struct gsink {
  uint8_t *buf;
  size_t capacity;
  size_t written;
};

static void gsink_put(struct gsink *s, const void *data, size_t len) {
  if (s->buf != NULL && s->written + len <= s->capacity) {
    memcpy(s->buf + s->written, data, len);
  }
  s->written += len;
}
static void gsink_put_u32(struct gsink *s, uint32_t v) { gsink_put(s, &v, 4); }
static void gsink_put_u64(struct gsink *s, uint64_t v) { gsink_put(s, &v, 8); }
static void gsink_put_str(struct gsink *s, const char *str, size_t len) {
  uint16_t l = (uint16_t)len;
  gsink_put(s, &l, 2);
  gsink_put(s, str, len);
}

static uint32_t serialize_graph_locked(struct gsink *sink) {
  uint32_t node_count = 0;
  for (struct gnode *n = registry_head; n != NULL; n = n->reg_next) {
    node_count++;
  }
  gsink_put_u32(sink, node_count);

  for (struct gnode *n = registry_head; n != NULL; n = n->reg_next) {
    gsink_put_u64(sink, n->id);
    gsink_put_str(sink, n->label, strlen(n->label));
    gsink_put_u64(sink, n->size);
    gsink_put(sink, n->data, n->size);

    gsink_put_u32(sink, n->edge_count);
    for (struct gedge *e = n->edges; e != NULL; e = e->next) {
      gsink_put_str(sink, e->name, strlen(e->name));
      gsink_put_u64(sink, e->target->id);
    }
  }

  uint32_t sstring_count = 0;
  for (uint32_t i = 0; i < SSTRING_MAX_ENTRIES; i++) {
    if (sstrings[i].used) {
      sstring_count++;
    }
  }
  gsink_put_u32(sink, sstring_count);
  for (uint32_t i = 0; i < SSTRING_MAX_ENTRIES; i++) {
    if (!sstrings[i].used) {
      continue;
    }
    gsink_put_str(sink, sstrings[i].name, strlen(sstrings[i].name));
    gsink_put_u64(sink, sstrings[i].anchor->id);
  }

  return node_count;
}

struct gsrc {
  const uint8_t *buf;
  size_t len;
  size_t pos;
  bool overrun;
};

static void gsrc_get(struct gsrc *s, void *out, size_t n) {
  if (s->overrun || s->pos + n > s->len) {
    s->overrun = true;
    memset(out, 0, n);
    return;
  }
  memcpy(out, s->buf + s->pos, n);
  s->pos += n;
}
static uint16_t gsrc_get_u16(struct gsrc *s) {
  uint16_t v;
  gsrc_get(s, &v, 2);
  return v;
}
static uint32_t gsrc_get_u32(struct gsrc *s) {
  uint32_t v;
  gsrc_get(s, &v, 4);
  return v;
}
static uint64_t gsrc_get_u64(struct gsrc *s) {
  uint64_t v;
  gsrc_get(s, &v, 8);
  return v;
}

static void gsrc_skip(struct gsrc *s, uint64_t n) {
  if (s->overrun || s->pos + n > s->len) {
    s->overrun = true;
    return;
  }
  s->pos += n;
}

static void gsrc_get_str(struct gsrc *s, char *out, size_t out_cap) {
  uint16_t len = gsrc_get_u16(s);
  if (s->overrun || s->pos + len > s->len) {
    s->overrun = true;
    out[0] = '\0';
    return;
  }
  size_t n = MIN((size_t)len, out_cap - 1);
  memcpy(out, s->buf + s->pos, n);
  out[n] = '\0';
  s->pos += len;
}

static void gsrc_get_into_node(struct gsrc *s, struct gnode *n, uint64_t len) {
  if (s->overrun || s->pos + len > s->len) {
    s->overrun = true;
    return;
  }
  graph_write(n, 0, s->buf + s->pos, (size_t)len);
  s->pos += len;
}

static struct gnode *graph_node_create_with_id(uint64_t id, const char *label) {
  struct gnode *n = slab_alloc(&gnode_cache);
  if (n == NULL) {
    return NULL;
  }
  if (label != NULL) {
    strncpy(n->label, label, sizeof(n->label) - 1);
  }

  uint64_t f = spinlock_acquire_irqsave(&graph_lock);
  n->id = id;
  n->reg_next = registry_head;
  registry_head = n;
  spinlock_release_irqrestore(&graph_lock, f);

  return n;
}

static bool deserialize_graph(const uint8_t *buf, size_t len) {
  struct gsrc s1 = {.buf = buf, .len = len};
  uint32_t node_count = gsrc_get_u32(&s1);

  for (uint32_t i = 0; i < node_count && !s1.overrun; i++) {
    uint64_t id = gsrc_get_u64(&s1);
    char label[GNODE_LABEL_MAX];
    gsrc_get_str(&s1, label, sizeof(label));
    uint64_t data_size = gsrc_get_u64(&s1);
    if (s1.overrun) {
      break;
    }

    struct gnode *n = graph_node_create_with_id(id, label);
    if (n == NULL) {
      kprintf("[graph] out of memory reconstructing node #%lu\n", id);
      return false;
    }
    gsrc_get_into_node(&s1, n, data_size);

    uint32_t edge_count = gsrc_get_u32(&s1);
    for (uint32_t e = 0; e < edge_count && !s1.overrun; e++) {
      char name[GEDGE_NAME_MAX];
      gsrc_get_str(&s1, name, sizeof(name));
      gsrc_get_u64(&s1);
    }
  }
  if (s1.overrun) {
    kprintf("[graph] corrupt/truncated snapshot (pass 1 -- node structure)\n");
    return false;
  }

  struct gsrc s2 = {.buf = buf, .len = len};
  uint32_t node_count2 = gsrc_get_u32(&s2);
  for (uint32_t i = 0; i < node_count2 && !s2.overrun; i++) {
    uint64_t id = gsrc_get_u64(&s2);
    char label[GNODE_LABEL_MAX];
    gsrc_get_str(&s2, label, sizeof(label));
    (void)label;
    uint64_t data_size = gsrc_get_u64(&s2);
    gsrc_skip(&s2, data_size);

    struct gnode *from = graph_find_by_id(id);
    uint32_t edge_count = gsrc_get_u32(&s2);
    for (uint32_t e = 0; e < edge_count && !s2.overrun; e++) {
      char name[GEDGE_NAME_MAX];
      gsrc_get_str(&s2, name, sizeof(name));
      uint64_t target_id = gsrc_get_u64(&s2);
      struct gnode *to = graph_find_by_id(target_id);
      if (from != NULL && to != NULL) {
        graph_link(from, name, to);
      }
    }
  }

  uint32_t sstring_count = gsrc_get_u32(&s2);
  for (uint32_t i = 0; i < sstring_count && !s2.overrun; i++) {
    char name[SSTRING_NAME_MAX];
    gsrc_get_str(&s2, name, sizeof(name));
    uint64_t anchor_id = gsrc_get_u64(&s2);
    struct gnode *anchor = graph_find_by_id(anchor_id);
    if (anchor != NULL) {
      sstring_set(name, anchor);
    }
  }

  if (s2.overrun) {
    kprintf("[graph] corrupt/truncated snapshot (pass 2 -- edges/sstrings)\n");
    return false;
  }
  return true;
}

/* ------------------------------ disk gate -------------------------------
 * fs/graph.c used to hand-roll its own CAS-spin-and-yield lock here,
 * serializing graph_save_to_disk()/graph_load_from_disk() against
 * each other and against the autosave task (main.c's
 * graph_autosave_task()) across each function's ENTIRE multi-step
 * body. That's gone now: drivers/blockdev.c's blockdev_read()/
 * blockdev_write() serialize themselves against any concurrent
 * caller, kernel-wide, at the level of a single call -- see that
 * file's own comment for why the guarantee has to live there instead
 * of here (every future caller of the HAL needs it, not just this
 * file's two functions).
 *
 * That changes the GRANULARITY of what's atomic below (each
 * blockdev_write()/blockdev_read() call is now its own critical
 * section, not the whole superblock-then-payload sequence as one
 * unit) but not the SAFETY of what ends up on disk. What actually
 * protects a concurrent gload() from a save landing in the middle of
 * it was always the checksum in struct graph_disk_superblock, not a
 * lock spanning both functions: if a save's writes and a load's reads
 * interleave, the load either sees the fully-old pairing (both stale,
 * self-consistent, checksum matches) or the fully-new one (both
 * fresh, checksum matches) -- or, in the narrow window where it reads
 * a superblock from one moment and payload bytes from another, the
 * checksum simply won't match, and graph_load_from_disk() already
 * does exactly the right thing there: refuses to load rather than
 * reconstructing a torn mix. See that function's checksum check
 * below. The realistic exposure to that window is small besides:
 * gload() already refuses to run against a non-empty in-memory graph,
 * so it's a rare, deliberate, single-shot shell action -- not
 * something racing autosave on every cycle the way two overlapping
 * saves could. */

bool graph_save_to_disk(void) {
  if (!blockdev_available()) {
    kprintf("[graph] no block device available -- can't save\n");
    return false;
  }

  /* Clear dirty BEFORE the snapshot, not after writing it to disk. A
   * mutation landing after this point just re-sets the flag (every
   * mutator does so unconditionally), so the worst case is one extra
   * autosave cycle later. Clearing after the write instead could
   * silently lose exactly that mutation: the snapshot already on
   * disk wouldn't have it, but the flag would say "clean" anyway. */
  graph_clear_dirty();

  uint64_t buf_pages = DIV_ROUND_UP(GRAPH_SNAPSHOT_MAX_BYTES, PAGE_SIZE);
  uint64_t payload_phys = pmm_alloc_pages(buf_pages);
  if (payload_phys == 0) {
    kprintf("[graph] out of memory allocating a %lu-page snapshot buffer\n",
            buf_pages);
    graph_mark_dirty();
    return false;
  }
  uint8_t *payload_buf = (uint8_t *)phys_to_virt(payload_phys);

  struct gsink sink = {
      .buf = payload_buf, .capacity = GRAPH_SNAPSHOT_MAX_BYTES, .written = 0};

  uint64_t f = spinlock_acquire_irqsave(&graph_lock);
  uint32_t node_count = serialize_graph_locked(&sink);
  uint64_t saved_next_id = next_node_id;
  spinlock_release_irqrestore(&graph_lock, f);

  if (sink.written > sink.capacity) {
    kprintf(
        "[graph] graph too large to snapshot (needs %lu bytes, v1 cap is %u)\n",
        (uint64_t)sink.written, (unsigned)GRAPH_SNAPSHOT_MAX_BYTES);
    pmm_free_pages(payload_phys, buf_pages);
    graph_mark_dirty();
    return false;
  }

  uint64_t payload_bytes = sink.written;
  uint32_t sector_size = blockdev_sector_size();
  uint32_t payload_sectors = (uint32_t)DIV_ROUND_UP(payload_bytes, sector_size);

  uint64_t sb_phys = pmm_alloc_page();
  if (sb_phys == 0) {
    kprintf("[graph] out of memory allocating the superblock buffer\n");
    pmm_free_pages(payload_phys, buf_pages);
    graph_mark_dirty();
    return false;
  }
  struct graph_disk_superblock *sb =
      (struct graph_disk_superblock *)phys_to_virt(sb_phys);
  memset(sb, 0, sizeof(*sb));
  memcpy(sb->magic, "NEXUSGFS", 8);
  sb->version = GRAPH_DISK_VERSION;
  sb->checksum = fnv1a32(payload_buf, payload_bytes);
  sb->payload_bytes = payload_bytes;
  sb->next_node_id = saved_next_id;

  bool ok =
      blockdev_write(GRAPH_DISK_LBA_PAYLOAD, payload_sectors, payload_buf) &&
      blockdev_write(GRAPH_DISK_LBA_SUPERBLOCK, 1, sb);

  pmm_free_pages(payload_phys, buf_pages);
  pmm_free_page(sb_phys);

  if (!ok) {
    kprintf("[graph] NVMe write failed while saving\n");
    graph_mark_dirty();
    return false;
  }

  kprintf("[graph] saved: %u node(s), %lu byte(s) payload\n", node_count,
          payload_bytes);
  return true;
}

bool graph_load_from_disk(void) {
  if (!blockdev_available()) {
    kprintf("[graph] no block device available -- can't load\n");
    return false;
  }
  if (registry_head != NULL) {
    kprintf("[graph] refusing to load: the in-memory graph already has "
            "node(s) -- run gclear first, then gload again\n");
    return false;
  }

  uint64_t sb_phys = pmm_alloc_page();
  if (sb_phys == 0) {
    kprintf("[graph] out of memory allocating the superblock buffer\n");
    return false;
  }
  struct graph_disk_superblock *sb =
      (struct graph_disk_superblock *)phys_to_virt(sb_phys);

  if (!blockdev_read(GRAPH_DISK_LBA_SUPERBLOCK, 1, sb)) {
    kprintf("[graph] failed reading the superblock\n");
    pmm_free_page(sb_phys);
    return false;
  }

  if (memcmp(sb->magic, "NEXUSGFS", 8) != 0) {
    kprintf("[graph] no saved graph found -- starting empty\n");
    pmm_free_page(sb_phys);
    return false;
  }
  if (sb->version != GRAPH_DISK_VERSION) {
    kprintf("[graph] saved graph is format version %u, this kernel expects "
            "%u -- skipping\n",
            sb->version, (unsigned)GRAPH_DISK_VERSION);
    pmm_free_page(sb_phys);
    return false;
  }
  if (sb->payload_bytes > GRAPH_SNAPSHOT_MAX_BYTES) {
    kprintf("[graph] saved graph claims %lu bytes, larger than this "
            "kernel's %u-byte cap -- refusing\n",
            sb->payload_bytes, (unsigned)GRAPH_SNAPSHOT_MAX_BYTES);
    pmm_free_page(sb_phys);
    return false;
  }

  uint64_t payload_bytes = sb->payload_bytes;
  uint32_t expected_checksum = sb->checksum;
  uint64_t saved_next_id = sb->next_node_id;
  pmm_free_page(sb_phys);

  uint32_t sector_size = blockdev_sector_size();
  uint32_t payload_sectors = (uint32_t)DIV_ROUND_UP(payload_bytes, sector_size);
  uint64_t buf_pages =
      DIV_ROUND_UP((uint64_t)payload_sectors * sector_size, PAGE_SIZE);

  uint64_t payload_phys = pmm_alloc_pages(buf_pages);
  if (payload_phys == 0) {
    kprintf("[graph] out of memory allocating a %lu-page load buffer\n",
            buf_pages);
    return false;
  }
  uint8_t *payload_buf = (uint8_t *)phys_to_virt(payload_phys);

  if (!blockdev_read(GRAPH_DISK_LBA_PAYLOAD, payload_sectors, payload_buf)) {
    kprintf("[graph] failed reading the saved graph payload\n");
    pmm_free_pages(payload_phys, buf_pages);
    return false;
  }

  if (fnv1a32(payload_buf, payload_bytes) != expected_checksum) {
    kprintf("[graph] saved graph failed its checksum (corrupt or "
            "partially written) -- refusing to load it\n");
    pmm_free_pages(payload_phys, buf_pages);
    return false;
  }

  bool ok = deserialize_graph(payload_buf, (size_t)payload_bytes);
  pmm_free_pages(payload_phys, buf_pages);

  if (!ok) {
    kprintf("[graph] load failed partway through -- graph may be "
            "partially reconstructed; gclear before trying again\n");
    return false;
  }

  uint64_t f = spinlock_acquire_irqsave(&graph_lock);
  next_node_id = saved_next_id;
  spinlock_release_irqrestore(&graph_lock, f);

  graph_clear_dirty();

  kprintf("[graph] loaded saved graph (%lu byte(s) payload)\n", payload_bytes);
  return true;
}
