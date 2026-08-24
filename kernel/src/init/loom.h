#ifndef NEXUS_LOOM_H
#define NEXUS_LOOM_H

#include "klib/klib.h"

/*
 * Loom -- Nexus's init/service-supervision system.
 *
 * There's no separate config format, no separate on-disk state, and
 * no new persistence code anywhere in this file: a supervised
 * service ("strand") IS a graph node (fs/graph.c), reachable at
 * "loom/<name>" exactly like any other graph path -- gtouch it into
 * existence, gwrite its descriptor, glink a dependency edge onto it,
 * done. `gsync`/`gload`/the autosave task already persist the ENTIRE
 * graph as one snapshot, so strand definitions ride along for free;
 * `ggc`/`gclear` already do mark-and-sweep GC with open-fd-style
 * pinning as a root mechanism (fs/graphfs_vfs.c's graph_node_retain()/
 * release()), so Loom just reuses that same pin instead of inventing
 * its own lifetime tracking.
 *
 * Strand descriptor format (a strand node's raw content, set via
 * `gwrite loom/<name> "..."` or any classic-VFS write against the
 * same path): plain `key=value` lines, no quoting, no nesting --
 * matches the "no floats, no heavy parsing, prove the boring version
 * first" posture the rest of this kernel already takes.
 *
 *   path=/bin/hello       (required -- the ELF to run; missing means
 *                           "not configured yet", silently retried on
 *                           every future scan, not an error)
 *   uid=1000               (optional, defaults to 0/root)
 *   respawn=always          (optional, defaults to "once" -- run once
 *                             and stop; "always" gets crash-loop
 *                             protection, see LOOM_MAX_RAPID_RESTARTS)
 *
 * A dependency is any outgoing edge FROM a strand's own node (not the
 * "loom" anchor's edges, which just enumerate every known strand) --
 * `glink loom/webserver needs loom/database` means "launch database
 * before webserver." The edge's NAME carries no meaning to Loom, only
 * its target does -- purely a human-readable label for whoever's
 * looking with `gls`, same convention every other edge in this kernel
 * already uses. This is launch-ORDER only, not a hard prerequisite
 * gate: if a dependency fails to spawn, its dependents are still
 * attempted afterward and will likely fail for their own reasons.
 * Making it a true gate would need Loom to understand "is this
 * service actually ready", not just "does the process exist" -- a
 * bigger feature than v1 needs; see docs/Design.md.
 */

#define LOOM_MAX_STRANDS 32

/* Zeroes Loom's in-memory strand-tracking table. Call once, early at
 * boot (main.c, alongside graph_init()) -- doesn't touch the graph
 * itself at all, so ordering relative to graph_init() doesn't
 * actually matter, it's just grouped there for readability. */
void loom_init(void);

/* Does the initial strand scan (equivalent to loom_reload(true)) and
 * starts the background supervisor task ("loomd") that respawns
 * crashed always-strands and periodically picks up newly defined
 * ones. Call exactly once, after the graph has had its chance to load
 * from disk (see main.c's blockdev_init_task()) -- calling it before
 * that would just see an empty graph, which is harmless (nothing to
 * launch yet) but means anything saved from a previous session
 * wouldn't come back up until the NEXT periodic reload tick instead
 * of immediately. */
void loom_boot(void);

/* Re-scans the "loom" sstring anchor for strand definitions not yet
 * tracked, then launches whatever currently needs launching --
 * discoveries from this call, plus (only when `force_retry_faulted`)
 * anything previously marked faulted, in dependency order. The
 * background supervisor always passes false (a broken strand doesn't
 * get hammered with a fresh attempt every poll tick forever); the
 * shell's `loom reload` and the initial loom_boot() call both pass
 * true (a human/boot explicitly asking Loom to look again gets every
 * faulted strand one fresh try). Returns how many strands this call
 * actually launched. Safe to call even if "loom" doesn't exist yet --
 * returns 0. */
uint32_t loom_reload(bool force_retry_faulted);

/* Signals every currently-running strand to exit (same sched_kill_task()
 * mechanism `kill` uses, with the same at-next-syscall caveat -- see
 * that function's own comment), waits up to a few seconds for them to
 * actually die, reaps whichever did, and returns either way. Never
 * blocks indefinitely: a strand that never makes another syscall
 * can't be allowed to hang the whole machine's shutdown. Call this
 * BEFORE the actual power-off/reset action -- see shell/shell.c's
 * cmd_shutdown()/cmd_reboot(). */
void loom_shutdown_all(void);

enum loom_strand_state {
  LOOM_STRAND_NEVER_STARTED,
  LOOM_STRAND_RUNNING,
  LOOM_STRAND_EXITED,  /* respawn=once, finished */
  LOOM_STRAND_FAULTED, /* spawn failed, crash-loop budget exceeded, or a
                           dependency cycle -- see loom_reload()'s own
                           comment for what clears this */
};

struct loom_strand_info {
  char name[64]; /* matches fs/graph.h's GEDGE_NAME_MAX -- loom.h
                    deliberately doesn't include fs/graph.h itself, to
                    keep this public struct decoupled from graph
                    internals, so the literal is kept in sync by hand
                    (both are small, both are "this is plenty for a
                    hobby OS", neither has moved since either was
                    written). */
  char path[128];
  uint32_t uid;
  bool respawn_always;
  enum loom_strand_state state;
  uint64_t pid; /* 0 if not currently running */
  int last_exit_code;
  uint32_t restart_count;
};

/* Fills `out` (capacity `max`) with a snapshot of every currently
 * known strand, in table order (not launch order). Returns how many
 * were written -- used by the shell's `loom` status command. */
uint32_t loom_snapshot(struct loom_strand_info *out, uint32_t max);

#endif /* NEXUS_LOOM_H */
