#include "fs/tmpfs.h"
#include "mm/heap.h"
#include "panic.h"

struct tmpfs_node {
    struct vnode vnode;           /* embedded; vnode.fs_data points back at this node */
    uint8_t  *data;               /* VNODE_FILE only */
    size_t    capacity;           /* VNODE_FILE only -- allocated size of `data` */
    struct tmpfs_node *children;  /* VNODE_DIR only: singly-linked list of entries */
    struct tmpfs_node *next;      /* sibling link within the parent's `children` list */
};

static struct vnode *tmpfs_lookup(struct vnode *dir, const char *name);
static struct vnode *tmpfs_create(struct vnode *dir, const char *name, enum vnode_type type);
static bool          tmpfs_readdir(struct vnode *dir, uint32_t index, char *name_out, size_t name_max);
static size_t        tmpfs_read(struct vnode *node, uint64_t offset, void *buf, size_t len);
static size_t        tmpfs_write(struct vnode *node, uint64_t offset, const void *buf, size_t len);

static const struct vnode_ops tmpfs_ops = {
    .read = tmpfs_read,
    .write = tmpfs_write,
    .lookup = tmpfs_lookup,
    .create = tmpfs_create,
    .readdir = tmpfs_readdir,
};

static struct tmpfs_node *tmpfs_alloc(const char *name, enum vnode_type type) {
    struct tmpfs_node *n = kzalloc(sizeof(struct tmpfs_node));
    if (n == NULL) {
        return NULL;
    }
    n->vnode.type = type;
    n->vnode.ops = &tmpfs_ops;
    n->vnode.fs_data = n;
    n->vnode.size = 0;
    strncpy(n->vnode.name, name, sizeof(n->vnode.name) - 1);
    return n;
}

struct vnode *tmpfs_create_root(void) {
    struct tmpfs_node *root = tmpfs_alloc("/", VNODE_DIR);
    if (root == NULL) {
        panic("tmpfs: out of memory creating the root directory");
    }
    return &root->vnode;
}

static struct vnode *tmpfs_lookup(struct vnode *dir, const char *name) {
    struct tmpfs_node *d = dir->fs_data;
    for (struct tmpfs_node *c = d->children; c != NULL; c = c->next) {
        if (strcmp(c->vnode.name, name) == 0) {
            return &c->vnode;
        }
    }
    return NULL;
}

static struct vnode *tmpfs_create(struct vnode *dir, const char *name, enum vnode_type type) {
    struct tmpfs_node *d = dir->fs_data;
    struct tmpfs_node *n = tmpfs_alloc(name, type);
    if (n == NULL) {
        return NULL;
    }
    n->next = d->children;
    d->children = n;
    return &n->vnode;
}

static bool tmpfs_readdir(struct vnode *dir, uint32_t index, char *name_out, size_t name_max) {
    struct tmpfs_node *d = dir->fs_data;
    uint32_t i = 0;
    for (struct tmpfs_node *c = d->children; c != NULL; c = c->next, i++) {
        if (i == index) {
            strncpy(name_out, c->vnode.name, name_max - 1);
            name_out[name_max - 1] = '\0';
            return true;
        }
    }
    return false;
}

static bool tmpfs_ensure_capacity(struct tmpfs_node *n, size_t needed) {
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
        memcpy(nd, n->data, n->vnode.size);
        kfree(n->data);
    }
    n->data = nd;
    n->capacity = new_cap;
    return true;
}

static size_t tmpfs_read(struct vnode *node, uint64_t offset, void *buf, size_t len) {
    struct tmpfs_node *n = node->fs_data;
    if (offset >= node->size) {
        return 0;
    }
    size_t avail = (size_t)(node->size - offset);
    size_t n_read = MIN(len, avail);
    memcpy(buf, n->data + offset, n_read);
    return n_read;
}

static size_t tmpfs_write(struct vnode *node, uint64_t offset, const void *buf, size_t len) {
    struct tmpfs_node *n = node->fs_data;
    uint64_t end = offset + len;

    if (!tmpfs_ensure_capacity(n, (size_t)end)) {
        return 0;
    }

    /* Zero-fill a gap created by writing past the current EOF (a
     * sparse-write "hole"), matching normal file semantics. */
    if (offset > node->size) {
        memset(n->data + node->size, 0, (size_t)(offset - node->size));
    }

    memcpy(n->data + offset, buf, len);
    if (end > node->size) {
        node->size = end;
    }
    return len;
}
