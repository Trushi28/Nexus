#ifndef NEXUS_VFS_H
#define NEXUS_VFS_H

#include "klib/klib.h"

/*
 * A small, generic virtual filesystem layer: a vnode tree with a
 * per-node operations table, so any concrete filesystem (today, just
 * fs/tmpfs.c) can plug in without the rest of the kernel caring how a
 * given file is actually stored. v1 mounts exactly one filesystem at
 * "/" -- there's no mount table yet, just a single root pointer -- but
 * the vnode_ops indirection is what a real mount table would dispatch
 * through, so adding one later doesn't mean redesigning this.
 */

enum vnode_type {
  VNODE_FILE,
  VNODE_DIR,
};

struct vnode;

struct vnode_ops {
  /* Reads up to `len` bytes starting at `offset` into `buf`. Returns
   * the number of bytes actually read (0 at EOF). */
  size_t (*read)(struct vnode *node, uint64_t offset, void *buf, size_t len);

  /* Writes `len` bytes at `offset`, growing the file if `offset+len`
   * reaches past the current end. Returns the number of bytes
   * actually written (less than `len` only on OOM). */
  size_t (*write)(struct vnode *node, uint64_t offset, const void *buf,
                  size_t len);

  /* Returns the child named `name` inside a directory node, or NULL
   * if there isn't one. */
  struct vnode *(*lookup)(struct vnode *dir, const char *name);

  /* Creates and returns a new child of the given type inside a
   * directory node, or NULL (already exists, OOM, ...). */
  struct vnode *(*create)(struct vnode *dir, const char *name,
                          enum vnode_type type);

  /* Fills `name_out` (a caller buffer of `name_max` bytes) with the
   * name of the `index`'th child of a directory node, in whatever
   * order the filesystem happens to keep them. Returns false once
   * `index` is past the last child. */
  bool (*readdir)(struct vnode *dir, uint32_t index, char *name_out,
                  size_t name_max);
};

struct vnode {
  enum vnode_type type;
  char name[64];
  uint64_t size; /* VNODE_FILE only; 0 for directories */
  const struct vnode_ops *ops;
  void *fs_data; /* filesystem-private node */
};

struct vfs_file {
  struct vnode *node;
  uint64_t offset;
};

/* Installs `root` as the single mounted root ("/"). Call once at boot
 * before anything tries to open a path. */
void vfs_set_root(struct vnode *root);
struct vnode *vfs_root(void);

/* Resolves an absolute, '/'-separated path to a vnode ("/" or ""
 * itself resolves to the root). Returns NULL if any path component is
 * missing. v1 has no working-directory concept -- every path is
 * absolute whether or not it starts with '/'. */
struct vnode *vfs_lookup_path(const char *path);

/* Like vfs_lookup_path(), but creates any missing intermediate path
 * components as directories, and the final component (if missing) as
 * `type`. The "mkdir -p" primitive the initrd unpacker uses. */
struct vnode *vfs_lookup_or_create(const char *path, enum vnode_type type);

bool vfs_open(const char *path, bool create, struct vfs_file **out);
void vfs_close(struct vfs_file *f);
size_t vfs_read(struct vfs_file *f, void *buf, size_t len);
size_t vfs_write(struct vfs_file *f, const void *buf, size_t len);
uint64_t vfs_file_size(struct vfs_file *f);

/* Lists directory `path`'s `index`'th entry into `name_out` (a buffer
 * of `name_max` bytes). Returns false once `index` is past the last
 * entry (or `path` doesn't resolve to a directory at all). */
bool vfs_readdir(const char *path, uint32_t index, char *name_out,
                 size_t name_max);

#endif /* NEXUS_VFS_H */
