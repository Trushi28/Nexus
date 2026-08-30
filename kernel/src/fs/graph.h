#ifndef NEXUS_GRAPH_H
#define NEXUS_GRAPH_H

#include "fs/vfs.h"
#include "klib/klib.h"

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

  struct gnode *release_next; // scratch worklist for release_cascade_locked()
  bool gc_marked;
  struct gnode *reg_next;

  /* Open classic-VFS fd count -- separate from refcount so
   * collect_cycles_locked()'s from-scratch sweep treats an open fd as
   * a GC root too, the same way an sstring anchor is one. */
  uint32_t vfs_open_count;
  uint32_t owner_uid;

  /* File-vs-directory as presented by fs/graphfs_vfs.c -- decided once
   * (first outgoing edge, or explicit mkdir) and never flips back. */
  enum vnode_type classic_type;

  struct vnode vfs_node; // embedded classic-VFS adapter, see graphfs_vfs.c
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

void graph_truncate(struct gnode *n);

/* Locked snapshot of a node's size/edge_count, for callers outside
 * graph.c that need a consistent read without racing graph_lock. */
void graph_node_snapshot(struct gnode *n, uint64_t *size_out,
                         uint32_t *edge_count_out);

/* Takes/releases an external (classic-VFS) reference -- makes an open
 * fd count like an edge or sstring anchor for refcount-based freeing. */
void graph_node_retain(struct gnode *n);
void graph_node_release(struct gnode *n);

struct gnode *graph_resolve(const char *path);
struct gnode *graph_touch(const char *path, bool *out_created);
bool graph_remove_path(const char *path);
struct gnode *graph_find_by_id(uint64_t id);
void graph_for_each_node(void (*fn)(struct gnode *n, void *arg), void *arg);

bool sstring_set(const char *name, struct gnode *anchor);
struct gnode *sstring_get(const char *name);
bool sstring_list(uint32_t index, char *name_out, size_t name_max);

void sstring_unset(const char *name);

bool graph_save_to_disk(void);
bool graph_load_from_disk(void);
bool graph_is_dirty(void);
#endif /* NEXUS_GRAPH_H */
