#include "fs/vfs.h"
#include "mm/heap.h"

static struct vnode *root_node;

void vfs_set_root(struct vnode *root) {
    root_node = root;
}

struct vnode *vfs_root(void) {
    return root_node;
}

#define VFS_MAX_DEPTH 16

/* Splits `path` into up to VFS_MAX_DEPTH '/'-separated components and
 * walks the vnode tree from the root, optionally creating whatever's
 * missing along the way. Backs both vfs_lookup_path() (create_missing
 * = false) and vfs_lookup_or_create(). An empty path (or just "/")
 * returns the root itself, having walked zero components. */
static struct vnode *walk(const char *path, bool create_missing, enum vnode_type leaf_type) {
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
    for (int i = 0; i < depth; i++) {
        bool is_last = (i == depth - 1);

        if (cur->type != VNODE_DIR || cur->ops->lookup == NULL) {
            return NULL;
        }

        struct vnode *child = cur->ops->lookup(cur, comps[i]);
        if (child == NULL) {
            if (!create_missing || cur->ops->create == NULL) {
                return NULL;
            }
            child = cur->ops->create(cur, comps[i], is_last ? leaf_type : VNODE_DIR);
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

bool vfs_open(const char *path, struct vfs_file **out) {
    struct vnode *n = vfs_lookup_path(path);
    if (n == NULL || n->type != VNODE_FILE) {
        return false;
    }
    struct vfs_file *f = kzalloc(sizeof(struct vfs_file));
    if (f == NULL) {
        return false;
    }
    f->node = n;
    f->offset = 0;
    *out = f;
    return true;
}

void vfs_close(struct vfs_file *f) {
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

uint64_t vfs_file_size(struct vfs_file *f) {
    return f->node->size;
}

bool vfs_readdir(const char *path, uint32_t index, char *name_out, size_t name_max) {
    struct vnode *dir = vfs_lookup_path(path);
    if (dir == NULL || dir->type != VNODE_DIR || dir->ops->readdir == NULL) {
        return false;
    }
    return dir->ops->readdir(dir, index, name_out, name_max);
}
