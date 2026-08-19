#include "fs/vfs.h"
#include "abi/syscall_nr.h"
#include "mm/heap.h"

static struct vnode *root_node;

void vfs_set_root(struct vnode *root) { root_node = root; }

struct vnode *vfs_root(void) { return root_node; }

/* See vfs_set_root_fallback()'s comment in vfs.h -- NULL (the default)
 * means no fallback is consulted, and every existing lookup/create
 * behaves exactly as before this existed. */
static struct vnode *root_fallback;

void vfs_set_root_fallback(struct vnode *fallback) { root_fallback = fallback; }

/* ------------------------------ mount table ------------------------------
 * Deliberately tiny and linear (VFS_MAX_MOUNTS entries, scanned start
 * to finish on every lookup) -- this kernel mounts a small, fixed
 * number of filesystems, not hundreds, so there's nothing here a hash
 * table would meaningfully speed up. Each mount's path is stored
 * pre-split into components (rather than as a raw string) so
 * find_mount() can compare it directly against the already-split
 * components walk() builds for the path being resolved, with no
 * string-prefix edge cases (partial component matches, redundant
 * slashes, etc) to worry about.
 * -------------------------------------------------------------------- */

struct vfs_mount_entry {
  bool used;
  char comps[VFS_MOUNT_MAX_DEPTH][64];
  int depth;
  struct vnode *root;
};
static struct vfs_mount_entry mounts[VFS_MAX_MOUNTS];

/* Splits `path` into up to `max_depth` '/'-separated components.
 * Structurally identical to walk()'s own tokenizer just below, kept
 * as its own copy rather than shared -- a mount path is a rare,
 * shallow, boot-time-ish operation, not worth threading a shared
 * helper's signature through walk()'s hot path for. */
static int split_path(const char *path, char comps[][64], int max_depth) {
  int depth = 0;
  const char *p = path;
  while (*p == '/') {
    p++;
  }
  while (*p != '\0' && depth < max_depth) {
    size_t n = 0;
    while (*p != '\0' && *p != '/') {
      if (n + 1 < 64) {
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
  return depth;
}

static bool comps_equal(char a[][64], int a_depth, char b[][64], int b_depth) {
  if (a_depth != b_depth) {
    return false;
  }
  for (int i = 0; i < a_depth; i++) {
    if (strcmp(a[i], b[i]) != 0) {
      return false;
    }
  }
  return true;
}

bool vfs_mount(const char *path, struct vnode *root) {
  char comps[VFS_MOUNT_MAX_DEPTH][64];
  int depth = split_path(path, comps, VFS_MOUNT_MAX_DEPTH);
  if (depth == 0) {
    return false; /* "/" itself -- use vfs_set_root() instead */
  }

  int free_slot = -1;
  for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
    if (!mounts[i].used) {
      if (free_slot < 0) {
        free_slot = i;
      }
      continue;
    }
    if (comps_equal(mounts[i].comps, mounts[i].depth, comps, depth)) {
      return false; /* already mounted exactly here */
    }
  }
  if (free_slot < 0) {
    return false; /* mount table full */
  }

  mounts[free_slot].used = true;
  mounts[free_slot].depth = depth;
  for (int i = 0; i < depth; i++) {
    strncpy(mounts[free_slot].comps[i], comps[i],
            sizeof(mounts[free_slot].comps[i]) - 1);
  }
  mounts[free_slot].root = root;
  return true;
}

bool vfs_unmount(const char *path) {
  char comps[VFS_MOUNT_MAX_DEPTH][64];
  int depth = split_path(path, comps, VFS_MOUNT_MAX_DEPTH);

  for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
    if (mounts[i].used &&
        comps_equal(mounts[i].comps, mounts[i].depth, comps, depth)) {
      mounts[i].used = false;
      return true;
    }
  }
  return false;
}

/* Finds the longest-matching mount whose component path is a prefix
 * of the first `depth` entries of `comps`. On a match, returns that
 * mount's root vnode and sets `*start_index` to how many leading
 * components of `comps` the mount already accounts for -- walk()
 * resumes from there instead of from index 0. Returns NULL (leaving
 * `*start_index` untouched) if nothing beyond the primary root
 * applies, which is the common case (no extra mounts registered). */
static struct vnode *find_mount(char comps[][64], int depth, int *start_index) {
  struct vfs_mount_entry *best = NULL;

  for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
    if (!mounts[i].used || mounts[i].depth > depth) {
      continue;
    }
    bool match = true;
    for (int c = 0; c < mounts[i].depth; c++) {
      if (strcmp(mounts[i].comps[c], comps[c]) != 0) {
        match = false;
        break;
      }
    }
    if (match && (best == NULL || mounts[i].depth > best->depth)) {
      best = &mounts[i];
    }
  }

  if (best == NULL) {
    return NULL;
  }
  *start_index = best->depth;
  return best->root;
}

#define VFS_MAX_DEPTH 16

/* Splits `path` into up to VFS_MAX_DEPTH '/'-separated components and
 * walks the vnode tree from the root, optionally creating whatever's
 * missing along the way. Backs both vfs_lookup_path() (create_missing
 * = false) and vfs_lookup_or_create(). An empty path (or just "/")
 * returns the root itself, having walked zero components. */
static struct vnode *walk(const char *path, bool create_missing,
                          enum vnode_type leaf_type) {
  if (root_node == NULL) {
    return NULL;
  }

  char comps[VFS_MAX_DEPTH][64];
  int depth = 0;

  const char *p = path;
  while (*p == '/') {
    p++;
  }
  while (*p != '\0' && depth < VFS_MAX_DEPTH) {
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

  struct vnode *cur = root_node;
  int start = 0;
  struct vnode *mount_root = find_mount(comps, depth, &start);
  if (mount_root != NULL) {
    cur = mount_root;
  }

  for (int i = start; i < depth; i++) {
    bool is_last = (i == depth - 1);
    bool at_primary_root = (i == 0 && cur == root_node);

    if (cur->type != VNODE_DIR || cur->ops->lookup == NULL) {
      return NULL;
    }

    struct vnode *child = cur->ops->lookup(cur, comps[i]);

    /* Root-level fallback: a name that isn't a real entry in the
     * primary root might still be reachable through root_fallback
     * (e.g. an sstring-anchored graph node) -- try it exactly like an
     * ordinary lookup, generically through vnode_ops, before giving
     * up. Only at the true top level: one component directly under
     * "/", with no mount already selected for this walk. Anywhere
     * deeper, whatever vnode we already resolved to owns dispatch
     * from here on. */
    if (child == NULL && at_primary_root && root_fallback != NULL &&
        root_fallback->ops->lookup != NULL) {
      child = root_fallback->ops->lookup(root_fallback, comps[i]);
    }

    if (child == NULL) {
      if (!create_missing) {
        return NULL;
      }
      enum vnode_type want = is_last ? leaf_type : VNODE_DIR;

      if (at_primary_root && root_fallback != NULL &&
          root_fallback->ops->create != NULL) {
        /* A brand-new top-level name defaults to the fallback, not
         * the primary root -- see vfs_set_root_fallback()'s comment
         * in vfs.h for why (and main.c's boot sequence for why this
         * is registered only after the initrd is already unpacked). */
        child = root_fallback->ops->create(root_fallback, comps[i], want);
      } else if (cur->ops->create != NULL) {
        child = cur->ops->create(cur, comps[i], want);
      }
      if (child == NULL) {
        return NULL;
      }
    }
    cur = child;
  }
  return cur;
}

struct vnode *vfs_lookup_path(const char *path) {
  return walk(path, false, VNODE_FILE);
}

struct vnode *vfs_lookup_or_create(const char *path, enum vnode_type type) {
  return walk(path, true, type);
}

bool vfs_open(const char *path, int flags, struct vfs_file **out) {
  bool create = (flags & O_CREAT) != 0;

  struct vnode *n = vfs_lookup_path(path);
  if (n == NULL) {
    if (!create) {
      return false;
    }
    n = vfs_lookup_or_create(path, VNODE_FILE);
    if (n == NULL) {
      return false;
    }
  }
  if (n->type != VNODE_FILE) {
    return false;
  }
  struct vfs_file *f = kzalloc(sizeof(struct vfs_file));
  if (f == NULL) {
    return false;
  }
  f->node = n;
  f->offset = 0;
  f->flags = flags;
  if (n->ops->open != NULL) {
    n->ops->open(n);
  }
  *out = f;
  return true;
}

void vfs_close(struct vfs_file *f) {
  if (f->node->ops->close != NULL) {
    f->node->ops->close(f->node);
  }
  kfree(f);
}

size_t vfs_read(struct vfs_file *f, void *buf, size_t len) {
  if (f->node->ops->read == NULL) {
    return 0;
  }
  size_t got = f->node->ops->read(f->node, f->offset, buf, len);
  f->offset += got;
  return got;
}

size_t vfs_write(struct vfs_file *f, const void *buf, size_t len) {
  if (f->node->ops->write == NULL) {
    return 0;
  }
  size_t put = f->node->ops->write(f->node, f->offset, buf, len);
  f->offset += put;
  return put;
}

uint64_t vfs_file_size(struct vfs_file *f) { return f->node->size; }

bool vfs_truncate(struct vfs_file *f) {
  if (f->node->ops->truncate == NULL) {
    return false;
  }
  if (!f->node->ops->truncate(f->node)) {
    return false;
  }
  f->offset = 0;
  return true;
}

bool vfs_readdir(const char *path, uint32_t index, char *name_out,
                 size_t name_max) {
  struct vnode *dir = vfs_lookup_path(path);
  if (dir == NULL || dir->type != VNODE_DIR) {
    return false;
  }

  if (dir == root_node && root_fallback != NULL &&
      root_fallback->ops->readdir != NULL) {
    /* Merge the primary root's own entries with root_fallback's --
     * tmpfs's first (by index), then the fallback's continuing where
     * tmpfs's left off, so e.g. an sstring-anchored "photos" shows up
     * in `ls /` right alongside "bin". Re-counts tmpfs's entries on
     * every call rather than caching the count, so this stays correct
     * as files come and go between readdir() calls -- O(index) work,
     * fine for the handful of entries either namespace holds. */
    uint32_t tmpfs_count = 0;
    if (dir->ops->readdir != NULL) {
      char scratch[64];
      while (dir->ops->readdir(dir, tmpfs_count, scratch, sizeof(scratch))) {
        tmpfs_count++;
      }
    }
    if (index < tmpfs_count) {
      return dir->ops->readdir(dir, index, name_out, name_max);
    }
    return root_fallback->ops->readdir(root_fallback, index - tmpfs_count,
                                       name_out, name_max);
  }

  if (dir->ops->readdir == NULL) {
    return false;
  }
  return dir->ops->readdir(dir, index, name_out, name_max);
}
