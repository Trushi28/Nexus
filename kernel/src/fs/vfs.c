#include "fs/vfs.h"
#include "abi/syscall_nr.h"
#include "mm/heap.h"
#include "mm/slab.h"
static struct slab_cache vfs_file_cache;

static struct vnode *root_node;

void vfs_set_root(struct vnode *root) { root_node = root; }

struct vnode *vfs_root(void) { return root_node; }

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

void vfs_init(void) {
  slab_cache_init(&vfs_file_cache, sizeof(struct vfs_file), "vfs_file");
}

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
 * = false, `create_uid` unused) and vfs_lookup_or_create() (every
 * freshly-created node is owned by `create_uid` -- see that
 * function's own comment in vfs.h). An empty path (or just "/")
 * returns the root itself, having walked zero components. */
static struct vnode *walk(const char *path, bool create_missing,
                          enum vnode_type leaf_type, uint32_t create_uid) {
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

    if (cur->ops->lookup == NULL) {
      /* Every filesystem's own .lookup (tmpfs_lookup(), graphfs_*_lookup())
       * is responsible for refusing descent into what it considers a
       * plain file -- deliberately NOT enforced here via cur->type.
       * tmpfs's type is fixed at creation; a graph node's is now also
       * stable once set (see struct gnode::classic_type in graph.h),
       * but neither filesystem's invariant belongs at this generic
       * layer -- pushing the decision down to each filesystem's own
       * ops is what lets a currently-childless-but-still-a-directory
       * node correctly accept a new child here. */
      return NULL;
    }

    struct vnode *child = cur->ops->lookup(cur, comps[i]);

    if (child == NULL) {
      if (!create_missing) {
        return NULL;
      }
      enum vnode_type want = is_last ? leaf_type : VNODE_DIR;
      if (cur->ops->create != NULL) {
        child = cur->ops->create(cur, comps[i], want, create_uid);
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
  return walk(path, false, VNODE_FILE, 0); /* create_uid unused --
                                               create_missing is false */
}

/* Component buffer width matches every other path tokenizer in this
 * codebase (walk()'s own `comps[VFS_MAX_DEPTH][64]` above, and the
 * shell-local copies this replaces) -- plenty for anything a person
 * types or a filesystem generates a name for. */
#define VFS_RESOLVE_COMP_MAX 64

void vfs_resolve_relative(const char *cwd, const char *in, char *out,
                          size_t out_max) {
  char combined[256];
  if (in[0] == '/') {
    strncpy(combined, in, sizeof(combined) - 1);
    combined[sizeof(combined) - 1] = '\0';
  } else {
    ksnprintf(combined, sizeof(combined), "%s/%s", cwd, in);
  }

  char comps[VFS_RESOLVE_MAX_DEPTH][VFS_RESOLVE_COMP_MAX];
  int depth = 0;

  const char *p = combined;
  while (*p == '/') {
    p++;
  }
  while (*p != '\0' && depth < VFS_RESOLVE_MAX_DEPTH) {
    size_t n = 0;
    while (*p != '\0' && *p != '/') {
      if (n + 1 < sizeof(comps[0])) {
        comps[depth][n++] = *p;
      }
      p++;
    }
    comps[depth][n] = '\0';

    if (strcmp(comps[depth], "..") == 0) {
      if (depth > 0) {
        depth--;
      }
    } else if (comps[depth][0] != '\0' && strcmp(comps[depth], ".") != 0) {
      depth++;
    }
    while (*p == '/') {
      p++;
    }
  }

  if (depth == 0) {
    strncpy(out, "/", out_max - 1);
    out[out_max - 1] = '\0';
    return;
  }

  size_t off = 0;
  out[0] = '\0';
  for (int i = 0; i < depth; i++) {
    int n = ksnprintf(out + off, out_max - off, "/%s", comps[i]);
    if (n < 0 || (size_t)n >= out_max - off) {
      break;
    }
    off += (size_t)n;
  }
}

struct vnode *vfs_lookup_or_create(const char *path, enum vnode_type type,
                                   uint32_t uid) {
  return walk(path, true, type, uid);
}

bool vfs_open_as(const char *path, int flags, uint32_t uid,
                 struct vfs_file **out) {
  bool create = (flags & O_CREAT) != 0;
  bool wants_write =
      ((flags & O_ACCMODE) != O_RDONLY) || (flags & O_TRUNC) != 0;

  struct vnode *n = vfs_lookup_path(path);
  bool pre_existing = (n != NULL);
  if (n == NULL) {
    if (!create) {
      return false;
    }
    n = vfs_lookup_or_create(path, VNODE_FILE, uid);
    if (n == NULL) {
      return false;
    }
  }
  if (n->type != VNODE_FILE) {
    return false;
  }
  if (wants_write && pre_existing && uid != 0 && n->owner_uid != uid) {
    /* Refused outright, not silently degraded to read-only -- see
     * struct vnode::owner_uid's comment in vfs.h. Everything else in
     * this function is byte-for-byte what vfs_open() always did; this
     * is the entire ownership boundary. */
    return false;
  }

  struct vfs_file *f = slab_alloc(&vfs_file_cache);
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

bool vfs_open(const char *path, int flags, struct vfs_file **out) {
  return vfs_open_as(path, flags, 0, out);
}

void vfs_close(struct vfs_file *f) {
  if (f->node->ops->close != NULL) {
    f->node->ops->close(f->node);
  }
  slab_free(&vfs_file_cache, f);
}

struct vfs_file *vfs_dup(struct vfs_file *f) {
  struct vfs_file *nf = slab_alloc(&vfs_file_cache);
  if (nf == NULL) {
    return NULL;
  }
  nf->node = f->node;
  nf->offset = f->offset;
  nf->flags = f->flags;
  if (nf->node->ops->open != NULL) {
    nf->node->ops->open(nf->node);
  }
  return nf;
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

  if (dir->ops->readdir == NULL) {
    return false;
  }
  return dir->ops->readdir(dir, index, name_out, name_max);
}
