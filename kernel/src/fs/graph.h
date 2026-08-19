#ifndef NEXUS_GRAPH_H
#define NEXUS_GRAPH_H

#include "klib/klib.h"
#include "fs/vfs.h"

#define GNODE_LABEL_MAX 64
#define GEDGE_NAME_MAX 64
#define SSTRING_NAME_MAX 32
#define SSTRING_MAX_ENTRIES 64
#define GRAPH_MAX_DEPTH 16

struct gnode;

struct gedge {
  char name[GEDGE_NAME_MAX];
  struct gnode *target;
  struct gedge *next;
};

struct gnode {
  uint64_t id;
  char label[GNODE_LABEL_MAX];
  uint8_t *data;
  uint64_t size;
  uint64_t capacity;

  struct gedge *edges;
  uint32_t edge_count;
  uint32_t refcount;

  struct gnode *release_next; /* intrusive link: the release cascade's
                                  own worklist (see release_cascade_locked()
                                  in graph.c) -- mirrors the pending_exit/
                                  parked_head pattern in sched/sched.c and
                                  cpu/cpu.h: an explicit heap-free worklist
                                  instead of recursion, so a long chain
                                  (A->B->C->...) being released can't blow
                                  the kernel stack. Scratch only -- has no
                                  meaning outside of an active cascade. */
  bool gc_marked;
  struct gnode *reg_next;

  uint32_t vfs_open_count; /* how many open classic-VFS fds
                               (fs/graphfs_vfs.c) currently hold this
                               node -- bumped by graph_node_retain()/
                               graph_node_release(), which also bump
                               the ordinary `refcount` above, so
                               grm/gunlink/sstringrm's existing
                               "refcount==0" checks already refuse to
                               free a node an open fd is using.
                               vfs_open_count exists on top of that
                               purely so collect_cycles_locked()'s
                               mark-and-sweep -- which does its own
                               from-scratch reachability scan and
                               never consults refcount at all -- knows
                               to treat a held-open node as a GC root
                               too, the same way an sstring anchor is
                               one. */

  struct vnode vfs_node; /* embedded classic-VFS adapter for this node
                             (see fs/graphfs_vfs.c) -- re-populated
                             fresh on every access through that
                             adapter, same trick tmpfs_node uses for
                             its own embedded vnode. Owned by this
                             allocation: freeing the gnode frees this
                             along with it, no separate teardown
                             needed. */
};

void graph_init(void);

struct gnode *graph_node_create(const char *label);

void graph_link(struct gnode *from, const char *edge_name, struct gnode *to);

void graph_unlink(struct gnode *from, const char *edge_name);

void graph_node_delete(struct gnode *n);
void graph_clear_all(void);
uint32_t graph_collect_cycles(void);

struct gnode *graph_edge_lookup(struct gnode *from, const char *name);
bool graph_list_edges(struct gnode *from, uint32_t index, char *name_out,
                      size_t name_max);

size_t graph_read(struct gnode *n, uint64_t offset, void *buf, size_t len);
size_t graph_write(struct gnode *n, uint64_t offset, const void *buf,
                   size_t len);

/* Resets a node's logical length to 0 -- same effect O_TRUNC has on a
 * classic file. The backing allocation (data/capacity) is left in
 * place, exactly like tmpfs_truncate()'s equivalent tradeoff, so a
 * later write reuses it instead of paying to free and reallocate. */
void graph_truncate(struct gnode *n);

/* Locked read of a node's current size and edge_count, for callers
 * outside graph.c (fs/graphfs_vfs.c) that need a consistent snapshot
 * of both without reaching into struct gnode directly and racing
 * graph_lock-protected mutations. Either output pointer may be NULL
 * if you only want the other. */
void graph_node_snapshot(struct gnode *n, uint64_t *size_out,
                         uint32_t *edge_count_out);

/* Takes/releases an external (classic-VFS) reference on a node --
 * called from fs/graphfs_vfs.c's open/close hooks, one retain per
 * successful vfs_open(), one release per matching vfs_close(). Makes
 * an open fd count exactly like an edge or sstring anchor for
 * refcount-based freeing (grm/gunlink/sstringrm already refuse a
 * nonzero-refcount node), and additionally marks the node as a GC
 * root for collect_cycles_locked(), which doesn't consult refcount at
 * all. graph_node_release() may free the node (same as any other
 * refcount hitting 0) if this was its last reference of any kind --
 * don't touch `n` again after calling it unless you independently
 * know something else still references it. */
void graph_node_retain(struct gnode *n);
void graph_node_release(struct gnode *n);

struct gnode *graph_resolve(const char *path);
struct gnode *graph_touch(const char *path, bool *out_created);
struct gnode *graph_find_by_id(uint64_t id);
void graph_for_each_node(void (*fn)(struct gnode *n, void *arg), void *arg);

bool sstring_set(const char *name, struct gnode *anchor);
struct gnode *sstring_get(const char *name);
bool sstring_list(uint32_t index, char *name_out, size_t name_max);

void sstring_unset(const char *name);

/* ------------------------------ persistence ------------------------- */

bool graph_save_to_disk(void);
bool graph_load_from_disk(void);
bool graph_is_dirty(void);
#endif /* NEXUS_GRAPH_H */
