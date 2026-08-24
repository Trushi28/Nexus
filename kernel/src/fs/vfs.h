#ifndef NEXUS_VFS_H
#define NEXUS_VFS_H

#include "klib/klib.h"

/*
 * A small, generic virtual filesystem layer: a vnode tree with a
 * per-node operations table, so any concrete filesystem (today, just
 * fs/tmpfs.c) can plug in without the rest of the kernel caring how a
 * given file is actually stored. The primary root ("/") is set once
 * via vfs_set_root(); additional filesystems can be grafted in at any
 * path with vfs_mount() (see below) -- resolution picks the longest
 * matching mount prefix, same idea as a real Unix mount table, just
 * without the ability to unmount something still busy.
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
   * directory node, or NULL (already exists, OOM, ...). `owner_uid`
   * is who a FRESHLY created node should be owned by (struct
   * vnode::owner_uid below) -- irrelevant if this call ends up
   * returning an already-existing node instead (every implementation
   * leaves that node's original owner untouched). */
  struct vnode *(*create)(struct vnode *dir, const char *name,
                          enum vnode_type type, uint32_t owner_uid);

  /* Fills `name_out` (a caller buffer of `name_max` bytes) with the
   * name of the `index`'th child of a directory node, in whatever
   * order the filesystem happens to keep them. Returns false once
   * `index` is past the last child. */
  bool (*readdir)(struct vnode *dir, uint32_t index, char *name_out,
                  size_t name_max);

  /* Truncates a file node to zero length. NULL for node types that
   * can't be truncated (directories -- there's no vnode_ops instance
   * without one today, but the pointer is still checked rather than
   * assumed, the same way every other optional op here is). */
  bool (*truncate)(struct vnode *node);

  /* Optional lifecycle hooks, not I/O: called once when a vfs_file is
   * created for this vnode (a successful vfs_open()) and once when
   * that vfs_file is closed (vfs_close()). NULL for filesystems
   * (tmpfs) that don't need per-open bookkeeping, since a tmpfs
   * node's lifetime isn't reclaimed while it's still reachable from
   * the tree anyway. Graph nodes (fs/graphfs_vfs.c) ARE reclaimed out
   * from under a path the moment their graph-level refcount hits
   * zero -- these hooks are how an open fd counts as one more such
   * reference, so grm/gunlink/ggc/gclear can't free a node a
   * classic-VFS handle is still using underneath it. vfs_dup() below
   * relies on these too, for exactly the same reason. */
  void (*open)(struct vnode *node);
  void (*close)(struct vnode *node);
};

struct vnode {
  enum vnode_type type;
  char name[64];
  uint64_t size; /* VNODE_FILE only; 0 for directories */
  const struct vnode_ops *ops;
  void *fs_data; /* filesystem-private node */

  /* ------------------------------ ownership -----------------------------
   * Who created this node -- 0 is root, and root bypasses every check
   * vfs_open_as() below makes. This is the WHOLE permission model:
   * one owner, no groups, no separate read/write/execute bits (there's
   * no concept of "execute" for a file at this layer anyway -- ELF
   * loading reads bytes like anything else). Read access is never
   * gated by this field at all -- there's no concept of a private file
   * in Nexus, only a protected one, since there's no login/session
   * concept to build real privacy on top of (see docs/Design.md).
   * Populated once, at creation, by whichever vnode_ops::create()
   * implementation actually allocated this node -- tmpfs
   * (fs/tmpfs.c) stores it directly on this persistent struct (never
   * touched again after creation); the graph filesystem
   * (fs/graphfs_vfs.c) mirrors it here from struct gnode::owner_uid
   * on every graphfs_wrap() call, since ITS vnode is rebuilt fresh on
   * every access rather than persisting like tmpfs's does. */
  uint32_t owner_uid;
};

struct vfs_file {
  struct vnode *node;
  uint64_t offset;
  int flags; /* the O_* flags (abi/syscall_nr.h) this was opened with --
                O_RDONLY/O_WRONLY/O_RDWR is all cpu/syscall.c's
                sys_read_impl()/sys_write_impl() actually check today. */
};

void vfs_init(void);
/* Installs `root` as the single mounted root ("/"). Call once at boot
 * before anything tries to open a path. */
void vfs_set_root(struct vnode *root);
struct vnode *vfs_root(void);

#define VFS_MAX_MOUNTS 8
#define VFS_MOUNT_MAX_DEPTH 4

/* Grafts `root` into the namespace at `path` (e.g. "/mnt/data"), so
 * any lookup under it resolves through `root` instead of the primary
 * root. The longest matching mount prefix wins if more than one could
 * apply. `path` itself is NOT auto-created as a directory in the
 * parent filesystem -- create it first (vfs_lookup_or_create()) if
 * you want it to show up in a readdir() of its parent, the same way a
 * real mount point is an ordinary directory until something's
 * mounted on it. Returns false if the mount table is full, `path` is
 * "/" itself (use vfs_set_root() for that) or too deep for
 * VFS_MOUNT_MAX_DEPTH components, or something is already mounted at
 * exactly that path. */
bool vfs_mount(const char *path, struct vnode *root);

/* Removes a mount previously installed at exactly `path`. Returns
 * false if nothing is mounted there. Doesn't touch anything mounted
 * *under* it -- unmount those first. */
bool vfs_unmount(const char *path);

/* Registers `fallback` as a second, transparent root: consulted, at
 * the TOP level only (a single path component directly under "/"),
 * whenever the primary root's own lookup/create doesn't have the
 * name -- no path prefix involved, unlike vfs_mount(). This is how
 * fs/graphfs_vfs.c's sstring-anchored graph nodes show up as ordinary
 * top-level entries (e.g. "photos") right alongside tmpfs's own,
 * rather than needing a dedicated mount point. Pass NULL to disable.
 * Entirely generic on this end -- vfs.c never assumes the fallback is
 * graphfs specifically, it just dispatches through vnode_ops like any
 * other vnode. */
void vfs_set_root_fallback(struct vnode *fallback);

/* Resolves an absolute, '/'-separated path to a vnode ("/" or ""
 * itself resolves to the root). Returns NULL if any path component is
 * missing. v1 has no working-directory concept -- every path is
 * absolute whether or not it starts with '/'. Never gated by
 * ownership -- see struct vnode::owner_uid's comment on why reading
 * is always unrestricted. */
struct vnode *vfs_lookup_path(const char *path);

/* Like vfs_lookup_path(), but creates any missing intermediate path
 * components as directories, and the final component (if missing) as
 * `type`. The "mkdir -p" primitive the initrd unpacker uses. Every
 * freshly-created node along the way (intermediate directories
 * included) is owned by `uid`; a node that already existed keeps
 * whatever owner it already had. Creation itself is never refused for
 * ownership reasons -- only a later WRITE to something you don't own
 * is (see vfs_open_as()) -- so `uid` here is purely "who gets to own
 * whatever ends up getting created", not a permission gate. */
struct vnode *vfs_lookup_or_create(const char *path, enum vnode_type type,
                                   uint32_t uid);

/* `flags` is the same O_* bitfield as abi/syscall_nr.h's open() --
 * O_CREAT creates a missing file, O_RDONLY/O_WRONLY/O_RDWR (extract
 * with `flags & O_ACCMODE`) is recorded on the resulting vfs_file for
 * later enforcement, and any other bit (O_TRUNC included) is stored
 * but otherwise ignored by vfs_open() itself -- see vfs_truncate()
 * for that one, called separately after open by whoever wants it.
 * Equivalent to vfs_open_as(path, flags, 0, out) below -- i.e. every
 * TRUSTED, kernel-internal caller (the initrd unpacker, process_spawn()
 * reading an ELF, the kernel shell's own cat/run/...) opens as root
 * and is therefore never refused by the ownership check. The one
 * caller that has to be honest about who's actually asking is the
 * open() syscall itself (cpu/syscall.c's sys_open_impl), which calls
 * vfs_open_as() directly with the calling ring-3 task's real uid. */
bool vfs_open(const char *path, int flags, struct vfs_file **out);

/* Same contract as vfs_open(), but checks ownership before granting a
 * write-capable handle -- see struct vnode::owner_uid's comment.
 * Concretely: opening for plain read (no O_WRONLY/O_RDWR/O_TRUNC) is
 * always allowed, for everyone, on anything that already exists.
 * Opening with O_WRONLY, O_RDWR, or O_TRUNC set against a node that
 * ALREADY EXISTED before this call is refused unless `uid == 0`
 * (root) or `uid` matches that node's owner. A brand new node
 * (O_CREAT, nothing was there before) is always created successfully
 * regardless of `uid` -- creation isn't gated, only tampering with
 * something someone else already made is -- and the new node is
 * owned by `uid` from that point on. */
bool vfs_open_as(const char *path, int flags, uint32_t uid,
                 struct vfs_file **out);

void vfs_close(struct vfs_file *f);
struct vfs_file *vfs_dup(struct vfs_file *f);

size_t vfs_read(struct vfs_file *f, void *buf, size_t len);
size_t vfs_write(struct vfs_file *f, const void *buf, size_t len);
uint64_t vfs_file_size(struct vfs_file *f);

bool vfs_truncate(struct vfs_file *f);

/* Lists directory `path`'s `index`'th entry into `name_out` (a buffer
 * of `name_max` bytes). Returns false once `index` is past the last
 * entry (or `path` doesn't resolve to a directory at all). */
bool vfs_readdir(const char *path, uint32_t index, char *name_out,
                 size_t name_max);

#endif /* NEXUS_VFS_H */
