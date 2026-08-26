#include "fs/graphfs_vfs.h"
#include "fs/graph.h"
#include "klib/klib.h"

/*
 * Adapts the graph filesystem (fs/graph.c) onto the classic vnode_ops
 * interface, so `ls`, `cat`, `run`, and ring-3's open() (including
 * O_CREAT/O_TRUNC) work against graph nodes exactly like they do
 * against tmpfs -- nothing else in the VFS layer needs to change,
 * which is the entire point of the vnode_ops indirection (see
 * fs/vfs.h's header comment).
 *
 * Wired up as the actual VFS root via vfs_set_root() (see main.c) --
 * GraphFS *is* "/" now, not a secondary fallback grafted alongside a
 * tmpfs root. An sstring anchor (`sstring photos <node>`) becomes an
 * ordinary top-level path -- "/photos" -- the instant it's set, with
 * no dedicated mount point needed. graphfs_vfs_root() (the bottom of
 * this file) is exactly "every current sstring anchor, listed as if
 * they were /'s own children."
 *
 * Graph nodes don't fit the strict Unix file-XOR-directory model: a
 * single node can carry both content (data/size) and outgoing edges
 * at once. This adapter resolves that with a type decided ONCE and
 * persisted on the gnode itself (struct gnode::classic_type, set in
 * graph.h) rather than recomputed on every access: VNODE_FILE by
 * default, promoted to VNODE_DIR permanently the instant the node
 * gains its first outgoing edge (graph_link()) or is explicitly
 * created as a directory through this adapter (an intermediate path
 * component, or an explicit mkdir-style vfs_lookup_or_create()) --
 * see graphfs_node_create()/graphfs_root_create() below. It never
 * flips back to VNODE_FILE, even if every edge is later gunlink'd --
 * same "an empty directory is still a directory" rule a real
 * filesystem follows. An EARLIER version of this adapter recomputed
 * the type from live edge_count on every single wrap, which meant the
 * SAME path could change shape as the graph evolved elsewhere (a node
 * that looked like a file the first time you `cat`'d it could refuse
 * to open at all the next, having silently become a directory) --
 * that's gone now. The one remaining wrinkle: a node that's both
 * non-empty AND has outgoing edges is still only reachable one way
 * (as whichever it was typed) through this adapter; the native
 * gcat/gls commands don't have that restriction, since they never
 * have to commit to a single vnode_type.
 *
 * Identity: each struct gnode owns its own embedded vfs_node (see
 * graph.h) instead of this file allocating a wrapper per lookup --
 * same trick tmpfs_node uses, for the same reason: stable identity,
 * and freeing the gnode automatically reclaims the embedded vnode's
 * memory too, no separate free path to maintain. graphfs_wrap()
 * re-populates it fresh on every access, so it's always current --
 * that includes owner_uid now (mirrored from the gnode's own
 * persistent field, same "never mutates after creation, safe to read
 * unprotected" reasoning `label` already relies on).
 *
 * Reference lifetime: graphfs_node_open()/graphfs_node_close() (the
 * .open/.close vnode_ops hooks, called once per vfs_open()/
 * vfs_close()) take and release a real graph reference via
 * graph_node_retain()/graph_node_release() -- an open classic-VFS fd
 * now counts exactly like an edge or sstring anchor, so grm/gunlink/
 * sstringrm/ggc/gclear can't free a node out from under a still-open
 * fd (see graph.c's collect_cycles_locked() for the ggc/gclear half
 * of that -- ordinary refcounting alone doesn't cover its from-
 * scratch mark-and-sweep). The root vnode itself has no .open/.close:
 * it's never opened as a file (always VNODE_DIR), so there's nothing
 * to hold a reference on.
 */

static const struct vnode_ops graphfs_node_ops;
static const struct vnode_ops graphfs_root_ops;

/* Re-populates `n`'s embedded vnode from its CURRENT graph state (via
 * the locked graph_node_snapshot() -- never peeking n->size/
 * n->edge_count directly, since those are graph_lock-protected fields
 * belonging to fs/graph.c, not this adapter) and returns it. `n->label`
 * is read unprotected: graph.c never mutates a label after node
 * creation, so there's no torn-read to guard against. `n->owner_uid`
 * is the same story. */
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
  node->size = size; /* keep the wrapper's cached size in sync */
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
    /* Pre-existing node keeps its original owner -- `owner_uid` here
     * only ever applies to a FRESH node, in the branch below. */
    n = existing;
  } else {
    n = graph_node_create(name);
    if (n == NULL) {
      return NULL;
    }
    n->owner_uid = owner_uid;
    graph_link(parent, name, n); /* also promotes `parent` to DIR, since
                                    it just gained an edge -- see
                                    graph_link()'s own comment */
  }

  if (type == VNODE_DIR) {
    /* walk() passes VNODE_DIR for every intermediate path component
     * (see its `is_last ? leaf_type : VNODE_DIR`) and for an explicit
     * mkdir-style vfs_lookup_or_create(..., VNODE_DIR, ...). Persist
     * it onto the gnode itself -- not just the wrapper this call is
     * about to return -- so it stays stable on every future access,
     * including one that finds this node with zero children so far
     * (e.g. an empty directory, or the split second before its first
     * child gets linked on the very next walk() iteration). See
     * struct gnode::classic_type's comment in graph.h. Silently
     * promoting an EXISTING node from file to directory here (rather
     * than refusing, the way a real mkdir would against a path that's
     * already a plain file) matches this adapter's existing stance
     * that a gnode isn't strictly one or the other -- see this file's
     * header comment. */
    n->classic_type = VNODE_DIR;
  }
  return graphfs_wrap(n);
}

static bool graphfs_node_readdir(struct vnode *dir, uint32_t index,
                                 char *name_out, size_t name_max) {
  return graph_list_edges((struct gnode *)dir->fs_data, index, name_out,
                          name_max);
}

/* Take/release a real graph reference for exactly as long as this fd
 * is open -- see graph_node_retain()/graph_node_release()'s own
 * comments for the mechanics. This is what closes the "dangling fd"
 * gap: grm/gunlink/sstringrm already refuse a nonzero-refcount node,
 * and ggc/gclear's mark-and-sweep now treats an open fd as a GC root
 * too (see graph.c's collect_cycles_locked()), so a node with a
 * classic-VFS handle still open on it survives every native deletion
 * path until that handle is closed. */
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

/* ------------------------------- root -------------------------------
 * The mount's root isn't a real gnode -- it's the virtual concept of
 * "every current sstring anchor" -- so it gets its own, separate ops
 * table dispatching straight to the sstring_* API instead of
 * graph_edge_lookup()/graph_list_edges(). A single static instance is
 * enough: there's exactly one graph filesystem, mounted at most once.
 * ------------------------------------------------------------------- */
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
      /* sstring table full -- same situation graph_touch() documents:
       * the node still gets created and returned so THIS call
       * succeeds, it just won't be reachable again by that name. */
    }
  }

  if (type == VNODE_DIR) {
    n->classic_type = VNODE_DIR; /* see graphfs_node_create()'s comment */
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
    /* no .read/.write/.truncate: the root is always VNODE_DIR (below),
     * and vfs_open() already refuses to open anything that isn't
     * VNODE_FILE, so these would never be reached. */
};

struct vnode *graphfs_vfs_root(void) {
  graphfs_root_vnode.type = VNODE_DIR;
  graphfs_root_vnode.ops = &graphfs_root_ops;
  strncpy(graphfs_root_vnode.name, "graph",
          sizeof(graphfs_root_vnode.name) - 1);
  graphfs_root_vnode.name[sizeof(graphfs_root_vnode.name) - 1] = '\0';
  graphfs_root_vnode.size = 0;
  graphfs_root_vnode.fs_data = NULL;
  graphfs_root_vnode.owner_uid = 0; /* never opened as a file -- see the
                                        vnode_ops table above -- but set
                                        explicitly for the same reason
                                        every other field here is */
  return &graphfs_root_vnode;
}
