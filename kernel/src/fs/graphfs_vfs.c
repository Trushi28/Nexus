#include "fs/graphfs_vfs.h"
#include "fs/graph.h"
#include "klib/klib.h"

/* Adapts fs/graph.c onto vnode_ops so ls/cat/run/open() work against
 * graph nodes exactly like tmpfs. Wired up as the real VFS root (see
 * main.c) -- an sstring anchor becomes an ordinary top-level path the
 * instant it's set. A gnode can carry both content and edges at once,
 * so file-vs-directory is decided once and persisted on
 * gnode::classic_type rather than recomputed per access (see graph.h). */

static const struct vnode_ops graphfs_node_ops;
static const struct vnode_ops graphfs_root_ops;

static struct vnode *graphfs_wrap(struct gnode *n) {
  uint64_t size;
  graph_node_snapshot(n, &size, NULL);

  n->vfs_node.type = n->classic_type;
  n->vfs_node.ops = &graphfs_node_ops;
  strncpy(n->vfs_node.name, n->label, sizeof(n->vfs_node.name) - 1);
  n->vfs_node.name[sizeof(n->vfs_node.name) - 1] = '\0';
  n->vfs_node.size = size;
  n->vfs_node.fs_data = n;
  n->vfs_node.owner_uid = n->owner_uid;
  return &n->vfs_node;
}

static size_t graphfs_node_read(struct vnode *node, uint64_t offset, void *buf,
                                size_t len) {
  return graph_read((struct gnode *)node->fs_data, offset, buf, len);
}

static size_t graphfs_node_write(struct vnode *node, uint64_t offset,
                                 const void *buf, size_t len) {
  struct gnode *n = (struct gnode *)node->fs_data;
  size_t put = graph_write(n, offset, buf, len);

  uint64_t size;
  graph_node_snapshot(n, &size, NULL);
  node->size = size;
  return put;
}

static bool graphfs_node_truncate(struct vnode *node) {
  struct gnode *n = (struct gnode *)node->fs_data;
  graph_truncate(n);
  node->size = 0;
  return true;
}

static struct vnode *graphfs_node_lookup(struct vnode *dir, const char *name) {
  struct gnode *child = graph_edge_lookup((struct gnode *)dir->fs_data, name);
  if (child == NULL) {
    return NULL;
  }
  return graphfs_wrap(child);
}

static struct vnode *graphfs_node_create(struct vnode *dir, const char *name,
                                         enum vnode_type type,
                                         uint32_t owner_uid) {
  struct gnode *parent = (struct gnode *)dir->fs_data;

  struct gnode *existing = graph_edge_lookup(parent, name);
  struct gnode *n;
  if (existing != NULL) {
    n = existing;
  } else {
    n = graph_node_create(name);
    if (n == NULL) {
      return NULL;
    }
    n->owner_uid = owner_uid;
    graph_link(parent, name, n);
  }

  if (type == VNODE_DIR) {
    n->classic_type = VNODE_DIR;
  }
  return graphfs_wrap(n);
}

static bool graphfs_node_readdir(struct vnode *dir, uint32_t index,
                                 char *name_out, size_t name_max) {
  return graph_list_edges((struct gnode *)dir->fs_data, index, name_out,
                          name_max);
}

// One retain per vfs_open(), one release per matching vfs_close().
static void graphfs_node_open(struct vnode *node) {
  graph_node_retain((struct gnode *)node->fs_data);
}

static void graphfs_node_close(struct vnode *node) {
  graph_node_release((struct gnode *)node->fs_data);
}

static const struct vnode_ops graphfs_node_ops = {
    .read = graphfs_node_read,
    .write = graphfs_node_write,
    .lookup = graphfs_node_lookup,
    .create = graphfs_node_create,
    .readdir = graphfs_node_readdir,
    .truncate = graphfs_node_truncate,
    .open = graphfs_node_open,
    .close = graphfs_node_close,
};

// The mount root is "every current sstring anchor", not a real gnode.
static struct vnode graphfs_root_vnode;

static struct vnode *graphfs_root_lookup(struct vnode *dir, const char *name) {
  (void)dir;
  struct gnode *n = sstring_get(name);
  if (n == NULL) {
    return NULL;
  }
  return graphfs_wrap(n);
}

static struct vnode *graphfs_root_create(struct vnode *dir, const char *name,
                                         enum vnode_type type,
                                         uint32_t owner_uid) {
  (void)dir;

  struct gnode *existing = sstring_get(name);
  struct gnode *n;
  if (existing != NULL) {
    n = existing;
  } else {
    n = graph_node_create(name);
    if (n == NULL) {
      return NULL;
    }
    n->owner_uid = owner_uid;
    if (!sstring_set(name, n)) {
      // Table full -- node is still created and returned, just unreachable by name.
    }
  }

  if (type == VNODE_DIR) {
    n->classic_type = VNODE_DIR;
  }
  return graphfs_wrap(n);
}

static bool graphfs_root_readdir(struct vnode *dir, uint32_t index,
                                 char *name_out, size_t name_max) {
  (void)dir;
  return sstring_list(index, name_out, name_max);
}

static const struct vnode_ops graphfs_root_ops = {
    .lookup = graphfs_root_lookup,
    .create = graphfs_root_create,
    .readdir = graphfs_root_readdir,
};

struct vnode *graphfs_vfs_root(void) {
  graphfs_root_vnode.type = VNODE_DIR;
  graphfs_root_vnode.ops = &graphfs_root_ops;
  strncpy(graphfs_root_vnode.name, "graph",
          sizeof(graphfs_root_vnode.name) - 1);
  graphfs_root_vnode.name[sizeof(graphfs_root_vnode.name) - 1] = '\0';
  graphfs_root_vnode.size = 0;
  graphfs_root_vnode.fs_data = NULL;
  graphfs_root_vnode.owner_uid = 0;
  return &graphfs_root_vnode;
}
