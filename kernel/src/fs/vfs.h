#ifndef NEXUS_VFS_H
#define NEXUS_VFS_H

#include "klib/klib.h"

/* Generic vnode-tree VFS layer: any filesystem plugs in via a
 * per-node vnode_ops table. Root is set once via vfs_set_root();
 * vfs_mount() grafts more in, resolving on longest-prefix match. */

enum vnode_type {
  VNODE_FILE,
  VNODE_DIR,
};

struct vnode;

struct vnode_ops {
  size_t (*read)(struct vnode *node, uint64_t offset, void *buf, size_t len);

  // Grows the file if offset+len passes the current end.
  size_t (*write)(struct vnode *node, uint64_t offset, const void *buf,
                  size_t len);

  struct vnode *(*lookup)(struct vnode *dir, const char *name);

  // owner_uid only applies if this call actually creates a new node.
  struct vnode *(*create)(struct vnode *dir, const char *name,
                          enum vnode_type type, uint32_t owner_uid);

  bool (*readdir)(struct vnode *dir, uint32_t index, char *name_out,
                  size_t name_max);

  bool (*truncate)(struct vnode *node);

  /* Lifecycle hooks, not I/O -- called once per vfs_open()/vfs_close().
   * NULL for filesystems (tmpfs) with nothing to track per-open. */
  void (*open)(struct vnode *node);
  void (*close)(struct vnode *node);
};

struct vnode {
  enum vnode_type type;
  char name[64];
  uint64_t size; // VNODE_FILE only
  const struct vnode_ops *ops;
  void *fs_data;

  /* Who created this node -- the whole permission model (no groups,
   * no read/write/execute bits). Read is never gated by this at all;
   * see vfs_open_as() for the one place it matters. */
  uint32_t owner_uid;
};

struct vfs_file {
  struct vnode *node;
  uint64_t offset;
  int flags; // the O_* flags this was opened with
};

void vfs_init(void);
void vfs_set_root(struct vnode *root);
struct vnode *vfs_root(void);

#define VFS_MAX_MOUNTS 8
#define VFS_MOUNT_MAX_DEPTH 4

/* Grafts `root` into the namespace at `path`. Doesn't auto-create
 * `path` itself -- create it first if you want it to show up in a
 * readdir() of its parent. False if the slot's taken or path is "/". */
bool vfs_mount(const char *path, struct vnode *root);

bool vfs_unmount(const char *path);

/* Resolves an absolute path ("/" or "" is the root). NULL if any
 * component is missing. Never gated by ownership. */
struct vnode *vfs_lookup_path(const char *path);

/* Joins `in` onto `cwd` (verbatim if already absolute) and normalizes
 * the result -- doesn't touch the VFS itself. The one cwd-aware entry
 * point; every lookup/open/create above stays absolute-path only. */
#define VFS_RESOLVE_MAX_DEPTH 16
void vfs_resolve_relative(const char *cwd, const char *in, char *out,
                          size_t out_max);

/* Like vfs_lookup_path(), but creates missing intermediate directories
 * and the final component as `type`, owned by `uid`. */
struct vnode *vfs_lookup_or_create(const char *path, enum vnode_type type,
                                   uint32_t uid);

/* Equivalent to vfs_open_as(path, flags, 0, out) -- every trusted,
 * kernel-internal caller opens as root. */
bool vfs_open(const char *path, int flags, struct vfs_file **out);

/* Checks ownership before granting a write-capable handle: opening
 * O_WRONLY/O_RDWR/O_TRUNC on a PRE-EXISTING node owned by someone else
 * is refused unless uid==0. A brand-new (O_CREAT) node is never gated. */
bool vfs_open_as(const char *path, int flags, uint32_t uid,
                 struct vfs_file **out);

void vfs_close(struct vfs_file *f);
struct vfs_file *vfs_dup(struct vfs_file *f);

size_t vfs_read(struct vfs_file *f, void *buf, size_t len);
size_t vfs_write(struct vfs_file *f, const void *buf, size_t len);
uint64_t vfs_file_size(struct vfs_file *f);

bool vfs_truncate(struct vfs_file *f);

bool vfs_readdir(const char *path, uint32_t index, char *name_out,
                 size_t name_max);

#endif /* NEXUS_VFS_H */
