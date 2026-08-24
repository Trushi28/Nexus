#include "init/loom.h"
#include "debug/log.h"
#include "fs/graph.h"
#include "klib/klib.h"
#include "proc/process.h"
#include "sched/sched.h"
#include "sync/spinlock.h"
#include "time/timer.h"

#define LOOM_DESC_MAX_BYTES 512
#define LOOM_POLL_MS 500
#define LOOM_RESPAWN_COOLDOWN_MS 3000
#define LOOM_MAX_RAPID_RESTARTS 5
#define LOOM_SHUTDOWN_TIMEOUT_MS 3000

struct loom_strand {
  bool used;
  uint64_t gnode_id; /* NEVER a raw struct gnode* -- an id is safe to
                         hold indefinitely (just a number), a pointer
                         wouldn't be if the pin below were ever wrong.
                         Re-resolved via graph_find_by_id() everywhere
                         it's needed instead. */
  char name[GEDGE_NAME_MAX];
  char path[128];
  uint32_t uid;
  bool respawn_always;
  struct task *task; /* NULL if not currently running */
  bool ever_launched;
  bool exited_once;
  bool faulted;
  int last_exit_code;
  uint32_t restart_count;
  uint64_t last_launch_ms;
};

static struct loom_strand strands[LOOM_MAX_STRANDS];

/* Ordering, for the rare case both are held at once (loom_lock is
 * held across several fs/graph.c calls, each of which transiently
 * takes graph_lock internally): loom_lock is ALWAYS outer, graph_lock
 * is ALWAYS inner -- same explicit-ordering discipline sched.c's own
 * wait_lock/registry_lock pair documents, for the identical reason
 * (nothing in fs/graph.c ever touches loom_lock, so there's only one
 * direction this can nest in, but stating it here means it stays that
 * way on purpose, not by accident). */
static spinlock_t loom_lock = SPINLOCK_INIT;

void loom_init(void) {
  memset(strands, 0, sizeof(strands));
  spinlock_init(&loom_lock);
}

/* ------------------------------ descriptor -------------------------- */

struct loom_descriptor {
  char path[128];
  uint32_t uid;
  bool respawn_always;
};

/* Hand-rolled key=value tokenizer -- no quoting, no escaping, no
 * nesting. Splits on ANY run of whitespace (space, tab, '\n', '\r'),
 * not just newlines: the kernel shell's line editor has no way to
 * type a literal embedded newline into a single command (Enter always
 * submits the line), and gwrite's own argument parsing doesn't strip
 * quotes either (see cmd_gwrite() in shell/shell.c), so a descriptor
 * MUST be authorable on one physical line for anyone to actually type
 * it in. Concretely:
 *
 *   gwrite loom/hello path=/bin/hello uid=0 respawn=once
 *
 * Do NOT wrap the value in quotes -- there's nothing here to strip
 * them, so a literal `"` just becomes part of whichever token it's
 * attached to and silently fails to match any known key. */
static void parse_descriptor(const char *text, struct loom_descriptor *out) {
  memset(out, 0, sizeof(*out));

  const char *p = text;
  while (*p != '\0') {
    while (*p != '\0' && (uint8_t)*p <= ' ') {
      p++; /* skip whitespace between tokens */
    }
    if (*p == '\0') {
      break;
    }
    const char *tok_start = p;
    while (*p != '\0' && (uint8_t)*p > ' ') {
      p++;
    }
    size_t toklen = (size_t)(p - tok_start);

    const char *eq = NULL;
    for (size_t i = 0; i < toklen; i++) {
      if (tok_start[i] == '=') {
        eq = &tok_start[i];
        break;
      }
    }
    if (eq == NULL) {
      continue; /* not a key=value token -- ignore rather than error,
                   same tolerant-of-stray-tokens posture as the rest
                   of this parser */
    }

    size_t keylen = (size_t)(eq - tok_start);
    const char *val = eq + 1;
    size_t vallen = toklen - keylen - 1;

    if (keylen == 4 && strncmp(tok_start, "path", 4) == 0) {
      size_t n = MIN(vallen, sizeof(out->path) - 1);
      memcpy(out->path, val, n);
      out->path[n] = '\0';
    } else if (keylen == 3 && strncmp(tok_start, "uid", 3) == 0) {
      uint32_t v = 0;
      for (size_t i = 0; i < vallen; i++) {
        if (val[i] < '0' || val[i] > '9') {
          break;
        }
        v = v * 10 + (uint32_t)(val[i] - '0');
      }
      out->uid = v;
    } else if (keylen == 7 && strncmp(tok_start, "respawn", 7) == 0) {
      out->respawn_always = (vallen == 6 && strncmp(val, "always", 6) == 0);
    }
  }
}

/* Reads a strand node's own content and parses it. Returns false if
 * there's no "path=" -- NOT treated as an error anywhere that calls
 * this: it just means the strand was gtouch'd but not gwrite'n yet,
 * and gets picked up plainly on the next scan once it is. */
static bool read_descriptor(struct gnode *n, struct loom_descriptor *out) {
  char buf[LOOM_DESC_MAX_BYTES];
  size_t got = graph_read(n, 0, buf, sizeof(buf) - 1);
  buf[got] = '\0';
  parse_descriptor(buf, out);
  return out->path[0] != '\0';
}

/* --------------------------- dependency ordering ---------------------- */

/* True if `node`'s OWN outgoing edges include one pointing at the
 * gnode `candidate_id` names -- see loom.h's header comment on why
 * every edge counts as a dependency regardless of its name. A
 * dependency naming something outside the current launch set
 * (already running, or not tracked as a strand at all) simply isn't
 * found here and is treated as already satisfied. */
static bool strand_depends_on(struct gnode *node, uint64_t candidate_id) {
  char name[GEDGE_NAME_MAX];
  uint32_t idx = 0;
  while (graph_list_edges(node, idx, name, sizeof(name))) {
    struct gnode *target = graph_edge_lookup(node, name);
    if (target != NULL && target->id == candidate_id) {
      return true;
    }
    idx++;
  }
  return false;
}

/* Caller holds loom_lock. Launches every strand slot named by
 * `pending[0..count)` (indices into `strands[]`) whose descriptor is
 * currently readable, in dependency order -- Kahn's algorithm,
 * iterative (not recursive), same "bounded explicit worklist over
 * recursion" preference fs/graph.c's own release_cascade_locked()
 * documents, for the same reason: nothing here should be able to blow
 * the kernel stack no matter how the dependency graph is shaped.
 * Whatever's left over after a cycle is detected gets marked faulted
 * with a log line explaining why, rather than launched in some
 * arbitrary order that might violate what was actually asked for.
 * Returns how many strands were successfully launched. */
static uint32_t launch_pending_locked(uint32_t *pending, uint32_t count) {
  struct loom_descriptor descs[LOOM_MAX_STRANDS];
  bool ready[LOOM_MAX_STRANDS] = {0};
  uint32_t indegree[LOOM_MAX_STRANDS] = {0};

  for (uint32_t k = 0; k < count; k++) {
    uint32_t i = pending[k];
    struct gnode *node = graph_find_by_id(strands[i].gnode_id);
    if (node == NULL) {
      /* Pinned via graph_node_retain() the moment it was first
       * discovered (see loom_reload()) -- this should be unreachable
       * as long as that pin holds, but a defensive "give up cleanly"
       * beats a NULL deref if that invariant is ever broken by a
       * future change. */
      strands[i].faulted = true;
      kprintf("[loom] '%s': definition node vanished -- marking faulted\n",
              strands[i].name);
      continue;
    }
    if (read_descriptor(node, &descs[k])) {
      ready[k] = true;
    }
    /* else: no path= yet -- silently pending, not a fault */
  }

  for (uint32_t k = 0; k < count; k++) {
    if (!ready[k]) {
      continue;
    }
    struct gnode *node = graph_find_by_id(strands[pending[k]].gnode_id);
    for (uint32_t j = 0; j < count; j++) {
      if (j == k || !ready[j]) {
        continue;
      }
      if (strand_depends_on(node, strands[pending[j]].gnode_id)) {
        indegree[k]++;
      }
    }
  }

  uint32_t launched = 0;
  bool done[LOOM_MAX_STRANDS] = {0};

  for (uint32_t round = 0; round < count; round++) {
    int32_t pick = -1;
    for (uint32_t k = 0; k < count; k++) {
      if (ready[k] && !done[k] && indegree[k] == 0) {
        pick = (int32_t)k;
        break;
      }
    }
    if (pick < 0) {
      break; /* nothing left with satisfied dependencies -- either
                everything ready has launched, or what remains is
                cyclic (handled below) */
    }
    done[pick] = true;

    uint32_t i = pending[pick];
    struct task *t =
        process_spawn(descs[pick].path, strands[i].name, descs[pick].uid);

    strands[i].uid = descs[pick].uid;
    strands[i].respawn_always = descs[pick].respawn_always;
    strncpy(strands[i].path, descs[pick].path, sizeof(strands[i].path) - 1);
    strands[i].path[sizeof(strands[i].path) - 1] = '\0';
    strands[i].last_launch_ms = timer_uptime_ms();

    if (t != NULL) {
      strands[i].task = t;
      strands[i].ever_launched = true;
      launched++;
      kprintf("[loom] '%s' launched as pid %lu (uid %u)\n", strands[i].name,
              t->id, descs[pick].uid);
    } else {
      strands[i].faulted = true;
      kprintf("[loom] '%s' failed to launch ('%s' -- bad ELF or out of "
              "memory)\n",
              strands[i].name, descs[pick].path);
    }

    for (uint32_t k = 0; k < count; k++) {
      if (!ready[k] || done[k]) {
        continue;
      }
      struct gnode *dep_node = graph_find_by_id(strands[pending[k]].gnode_id);
      if (strand_depends_on(dep_node, strands[i].gnode_id)) {
        indegree[k]--;
      }
    }
  }

  for (uint32_t k = 0; k < count; k++) {
    if (ready[k] && !done[k]) {
      strands[pending[k]].faulted = true;
      kprintf("[loom] '%s' is part of a dependency cycle -- not launched\n",
              strands[pending[k]].name);
    }
  }

  return launched;
}

/* -------------------------------- scanning ---------------------------- */

uint32_t loom_reload(bool force_retry_faulted) {
  uint64_t f = spinlock_acquire_irqsave(&loom_lock);

  struct gnode *root = sstring_get("loom");
  if (root == NULL) {
    spinlock_release_irqrestore(&loom_lock, f);
    return 0; /* no "loom" anchor exists yet -- nothing defined at all */
  }

  char edge_name[GEDGE_NAME_MAX];
  uint32_t idx = 0;
  while (graph_list_edges(root, idx, edge_name, sizeof(edge_name))) {
    struct gnode *target = graph_edge_lookup(root, edge_name);
    idx++;
    if (target == NULL) {
      continue;
    }

    bool already_tracked = false;
    for (uint32_t i = 0; i < LOOM_MAX_STRANDS; i++) {
      if (strands[i].used && strands[i].gnode_id == target->id) {
        already_tracked = true;
        break;
      }
    }
    if (already_tracked) {
      continue;
    }

    int32_t slot = -1;
    for (uint32_t i = 0; i < LOOM_MAX_STRANDS; i++) {
      if (!strands[i].used) {
        slot = (int32_t)i;
        break;
      }
    }
    if (slot < 0) {
      kprintf("[loom] '%s': no free strand slot (%u max) -- ignoring\n",
              edge_name, LOOM_MAX_STRANDS);
      continue;
    }

    struct loom_strand *s = &strands[slot];
    memset(s, 0, sizeof(*s));
    s->used = true;
    s->gnode_id = target->id;
    strncpy(s->name, edge_name, sizeof(s->name) - 1);

    /* Pin it -- the SAME graph_node_retain() an open classic-VFS fd
     * uses (fs/graphfs_vfs.c), which also makes it a GC root for
     * ggc/gclear's mark-and-sweep (see fs/graph.c's
     * collect_cycles_locked()) for free, no changes to graph.c
     * needed. Never released in v1 -- there's no "loom forget"
     * command yet, so a strand definition, once discovered, stays
     * pinned for the rest of this boot session even after
     * `gunlink`ing it from the "loom" anchor. Documented, not hidden
     * -- see docs/Design.md. */
    graph_node_retain(target);
    kprintf("[loom] discovered strand '%s' (#%lu)\n", s->name, target->id);
  }

  uint32_t pending[LOOM_MAX_STRANDS];
  uint32_t pending_count = 0;
  for (uint32_t i = 0; i < LOOM_MAX_STRANDS; i++) {
    if (!strands[i].used || strands[i].task != NULL || strands[i].exited_once) {
      continue;
    }
    if (strands[i].faulted) {
      if (!force_retry_faulted) {
        continue;
      }
      strands[i].faulted = false;
      strands[i].restart_count = 0;
    }
    pending[pending_count++] = i;
  }

  uint32_t launched = 0;
  if (pending_count > 0) {
    launched = launch_pending_locked(pending, pending_count);
  }

  spinlock_release_irqrestore(&loom_lock, f);
  return launched;
}

/* ------------------------------ supervision ---------------------------- */

static void loom_supervisor_task(void *arg) {
  (void)arg;
  for (;;) {
    sched_sleep_ms(LOOM_POLL_MS);

    uint64_t f = spinlock_acquire_irqsave(&loom_lock);
    for (uint32_t i = 0; i < LOOM_MAX_STRANDS; i++) {
      if (!strands[i].used || strands[i].task == NULL) {
        continue;
      }
      if (!sched_task_is_dead(strands[i].task)) {
        continue;
      }

      /* Capture first, reap second -- same ordering shell.c's own
       * reap_finished_jobs() uses, for the same reason (process_wait()
       * frees the struct task; touching strands[i].task again after
       * would be a use-after-free). Guaranteed non-blocking here,
       * since sched_task_is_dead() already confirmed TASK_DEAD --
       * sched_wait_task() takes its immediate-reap path for that case
       * -- but the lock is still dropped around the call anyway,
       * defensively, in case that ever changes. */
      struct task *dead = strands[i].task;
      strands[i].task = NULL;
      spinlock_release_irqrestore(&loom_lock, f);
      int code = process_wait(dead);
      f = spinlock_acquire_irqsave(&loom_lock);

      strands[i].last_exit_code = code;

      if (!strands[i].respawn_always) {
        strands[i].exited_once = true;
        kprintf("[loom] '%s' finished (code %d)\n", strands[i].name, code);
        continue;
      }

      uint64_t now = timer_uptime_ms();
      if (now - strands[i].last_launch_ms < LOOM_RESPAWN_COOLDOWN_MS) {
        strands[i].restart_count++;
      } else {
        strands[i].restart_count = 1;
      }

      if (strands[i].restart_count > LOOM_MAX_RAPID_RESTARTS) {
        strands[i].faulted = true;
        kprintf("[loom] '%s' crash-looped %u times within %u ms -- giving "
                "up (see 'loom' for status; 'loom reload' gives it one "
                "more try)\n",
                strands[i].name, strands[i].restart_count,
                LOOM_RESPAWN_COOLDOWN_MS);
        continue;
      }

      struct task *t =
          process_spawn(strands[i].path, strands[i].name, strands[i].uid);
      strands[i].last_launch_ms = timer_uptime_ms();
      if (t != NULL) {
        strands[i].task = t;
        kprintf("[loom] '%s' exited (code %d), respawned as pid %lu\n",
                strands[i].name, code, t->id);
      } else {
        strands[i].faulted = true;
        kprintf("[loom] '%s' exited (code %d), respawn failed\n",
                strands[i].name, code);
      }
    }
    spinlock_release_irqrestore(&loom_lock, f);

    loom_reload(false); /* pick up anything newly gtouch'd/gwrite'n since
                            the last tick -- never retries a fault on its
                            own, see this function's own comment above */
  }
}

void loom_boot(void) {
  uint32_t launched = loom_reload(true);
  kprintf("[loom] boot scan complete -- %u strand(s) launched\n", launched);
  task_create("loomd", loom_supervisor_task, NULL);
}

void loom_shutdown_all(void) {
  struct task *to_wait[LOOM_MAX_STRANDS];
  uint32_t n = 0;

  uint64_t f = spinlock_acquire_irqsave(&loom_lock);
  for (uint32_t i = 0; i < LOOM_MAX_STRANDS; i++) {
    if (strands[i].used && strands[i].task != NULL) {
      sched_kill_task(strands[i].task);
      to_wait[n++] = strands[i].task;
    }
  }
  spinlock_release_irqrestore(&loom_lock, f);

  if (n == 0) {
    return;
  }
  kprintf("[loom] signalled %u strand(s) to exit, waiting...\n", n);

  /* Same at-next-syscall caveat every kill_requested check has (see
   * cpu/syscall.c's syscall_dispatch()) -- a strand in a tight loop
   * with no syscalls at all won't actually stop until it eventually
   * makes one, or until this timeout gives up and shutdown proceeds
   * anyway. Never blocks forever: a hung strand must not be able to
   * hang the whole machine's shutdown. */
  uint64_t deadline = timer_uptime_ms() + LOOM_SHUTDOWN_TIMEOUT_MS;
  bool all_dead = false;
  while (!all_dead && timer_uptime_ms() < deadline) {
    all_dead = true;
    for (uint32_t i = 0; i < n; i++) {
      if (!sched_task_is_dead(to_wait[i])) {
        all_dead = false;
      }
    }
    if (!all_dead) {
      sched_sleep_ms(20);
    }
  }

  uint64_t f2 = spinlock_acquire_irqsave(&loom_lock);
  for (uint32_t i = 0; i < LOOM_MAX_STRANDS; i++) {
    if (strands[i].used && strands[i].task != NULL &&
        sched_task_is_dead(strands[i].task)) {
      struct task *dead = strands[i].task;
      strands[i].task = NULL;
      spinlock_release_irqrestore(&loom_lock, f2);
      strands[i].last_exit_code = process_wait(dead);
      f2 = spinlock_acquire_irqsave(&loom_lock);
    }
  }
  spinlock_release_irqrestore(&loom_lock, f2);

  kprintf("[loom] shutdown wait complete\n");
}

uint32_t loom_snapshot(struct loom_strand_info *out, uint32_t max) {
  uint64_t f = spinlock_acquire_irqsave(&loom_lock);
  uint32_t n = 0;
  for (uint32_t i = 0; i < LOOM_MAX_STRANDS && n < max; i++) {
    if (!strands[i].used) {
      continue;
    }
    struct loom_strand_info *o = &out[n];
    strncpy(o->name, strands[i].name, sizeof(o->name) - 1);
    o->name[sizeof(o->name) - 1] = '\0';
    strncpy(o->path, strands[i].path, sizeof(o->path) - 1);
    o->path[sizeof(o->path) - 1] = '\0';
    o->uid = strands[i].uid;
    o->respawn_always = strands[i].respawn_always;
    o->pid = (strands[i].task != NULL) ? strands[i].task->id : 0;
    o->last_exit_code = strands[i].last_exit_code;
    o->restart_count = strands[i].restart_count;
    if (strands[i].task != NULL) {
      o->state = LOOM_STRAND_RUNNING;
    } else if (strands[i].faulted) {
      o->state = LOOM_STRAND_FAULTED;
    } else if (strands[i].exited_once) {
      o->state = LOOM_STRAND_EXITED;
    } else {
      o->state = LOOM_STRAND_NEVER_STARTED;
    }
    n++;
  }
  spinlock_release_irqrestore(&loom_lock, f);
  return n;
}
