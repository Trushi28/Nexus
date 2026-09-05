#include "init/power.h"
#include "acpi/acpi.h"
#include "cpu/io.h"
#include "debug/log.h"
#include "fs/graph.h"
#include "sched/sched.h"
#include "time/timer.h"

/*
 * Loom used to own "signal every supervised strand, wait for them to
 * die, reap them" as part of clean shutdown. Service supervision has
 * moved to userland /bin/init (see userland/init.c), so the kernel no
 * longer has a privileged view into any one supervisor's service
 * table to coordinate with directly. This replaces loom_shutdown_all()
 * with a strictly more general version: signal EVERY live ring-3
 * task -- which naturally covers init itself and everything it
 * spawned (nsh, background jobs, anything), not just tasks one
 * particular supervisor happened to be tracking -- then wait up to a
 * timeout for the graph to reach a quiescent state before saving it.
 *
 * Deliberately does NOT reap (sched_wait_task()) anything here --
 * only ever checks liveness, re-resolving each task by id through
 * sched_find_waitable_task() rather than holding a raw struct task*
 * across time. Reaping is left to whichever task already owns that
 * job for a given task: init reaps its own children via its normal
 * supervision loop (userland/init.c), and main.c's init_task() reaps
 * /bin/init itself via the process_wait() call already in its own
 * restart loop. Reaping from here too would race against whichever of
 * those is concurrently reaping the very same task, and this kernel
 * has no per-task lock or generation counter to make that race
 * provably safe against a genuine use-after-free -- the existing
 * syscall ABI already has a version of this gap (SYS_wait has no
 * ownership check tying a caller to its actual children), so this
 * isn't introducing a new hazard, just declining to add another one.
 * Skipping the reap here costs nothing real: the machine is moments
 * from a reset or power-off either way, so a handful of zombie
 * struct task allocations never get a chance to matter.
 *
 * Same at-next-syscall caveat sched_kill_task() has always had -- a
 * task in a tight loop making no syscalls at all won't actually stop
 * until it eventually makes one, or until this timeout gives up and
 * shutdown proceeds regardless. Never blocks forever: a hung task
 * must not be able to hang the whole machine's shutdown.
 *
 * Also deliberately NOT a dependency-respecting sequenced shutdown --
 * Loom's own loom_shutdown_all() signalled every strand simultaneously
 * too, so this isn't a regression, just the same bluntness widened to
 * cover every user task instead of only ones a supervisor knew about.
 */
#define SHUTDOWN_WAIT_TIMEOUT_MS 3000
#define SHUTDOWN_MAX_TRACKED 64 /* see the array's own comment below */

struct shutdown_collect_ctx {
  uint64_t ids[SHUTDOWN_MAX_TRACKED];
  uint32_t count;
};

static void shutdown_signal_one(struct task *t, void *arg) {
  if (!t->is_user || !t->waitable) {
    return; /* kernel-side tasks (gautosave, idle, ...) aren't ring-3 services */
  }
  sched_kill_task(t);

  struct shutdown_collect_ctx *ctx = (struct shutdown_collect_ctx *)arg;
  if (ctx->count < SHUTDOWN_MAX_TRACKED) {
    ctx->ids[ctx->count++] = t->id;
  }
  /* Beyond the cap: still signalled above, just not individually
   * tracked for the wait loop below -- harmless this close to
   * power-off, and matches every other small fixed-size table already
   * in this kernel (the old LOOM_MAX_STRANDS, MAX_JOBS, PROC_MAX_FDS,
   * ...). */
}

static void shutdown_user_tasks(void) {
  struct shutdown_collect_ctx ctx = {.count = 0};
  sched_for_each_task(shutdown_signal_one, &ctx);

  if (ctx.count == 0) {
    return;
  }
  kprintf("[power] signalled %u task(s) to exit, waiting...\n", ctx.count);

  uint64_t deadline = timer_uptime_ms() + SHUTDOWN_WAIT_TIMEOUT_MS;
  for (;;) {
    bool any_alive = false;
    for (uint32_t i = 0; i < ctx.count; i++) {
      /* Re-resolved by id every pass rather than held as a pointer --
       * NULL just means it's already gone (dead and reaped by
       * whichever task actually owns it), which is exactly the
       * terminal state this loop is waiting for. */
      struct task *t = sched_find_waitable_task(ctx.ids[i]);
      if (t != NULL && !sched_task_is_dead(t)) {
        any_alive = true;
      }
    }
    if (!any_alive || timer_uptime_ms() >= deadline) {
      break;
    }
    sched_sleep_ms(20);
  }
  kprintf("[power] shutdown wait complete\n");
}

NORETURN void power_reboot(void) {
  shutdown_user_tasks();
  if (graph_is_dirty()) {
    kprintf("graph has unsaved changes -- saving before reboot...\n");
    graph_save_to_disk(); /* logs its own outcome; a failed save still
                              falls through to reboot -- refusing to
                              reboot over a save failure would be
                              worse */
  }
  kprintf("rebooting...\n");
  timer_busy_wait_ms(50);
  uint8_t status;
  do {
    status = inb(0x64);
  } while (status & 0x02);
  outb(0x64, 0xFE);
  hang();
}

NORETURN void power_shutdown(void) {
  shutdown_user_tasks();
  if (graph_is_dirty()) {
    kprintf("graph has unsaved changes -- saving before shutdown...\n");
    graph_save_to_disk();
  }
  kprintf("shutting down...\n");
  timer_busy_wait_ms(50);
  acpi_shutdown(); /* never returns -- real ACPI / QEMU-hack / halt
                       fallback chain lives in acpi.c */
}
