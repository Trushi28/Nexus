#ifndef NEXUS_SCHED_H
#define NEXUS_SCHED_H

#include "klib/klib.h"
#include "sync/spinlock.h"

enum task_state {
  TASK_READY,
  TASK_RUNNING,
  TASK_SLEEPING,
  TASK_BLOCKED,
  TASK_EXITING,
  TASK_DEAD,
};
typedef void (*task_entry_t)(void *arg);

struct vfs_file;
#define PROC_MAX_FDS 16
#define TASK_CWD_MAX 128

struct task {
  uint64_t rsp; /* only valid while NOT the running task */
  uint64_t kernel_stack_phys;
  size_t kernel_stack_pages;

  uint64_t id;
  uint32_t uid; /* this task's clearance -- 0 is root/unrestricted and
                   bypasses every ownership check in the kernel (see
                   cpu/syscall.c's sys_kill_impl for the first real
                   consumer). Kernel tasks (task_create()) are always
                   uid 0 -- they're compiled into the kernel image
                   itself, not attacker-influenced content, so there's
                   nothing to restrict them from. A ring-3 task
                   (task_create_user()) is handed an explicit uid at
                   creation time by whoever's spawning it: process_spawn()
                   threads one through from its own caller (see that
                   function's own comment), split()/exec() (cpu/syscall.c)
                   both inherit the calling task's CURRENT uid unchanged
                   -- exactly like a real fork/exec's default, no
                   POSIX setuid-binary equivalent exists here. The only
                   way a task's uid ever changes after creation is a
                   successful sys_setuid_impl() call, which is
                   deliberately one-way (root can drop to anything;
                   nothing can climb back up) -- see abi/syscall_nr.h's
                   SYS_setuid comment. */
  char name[32];
  enum task_state state;

  task_entry_t entry;
  void *arg;

  uint64_t wake_time_ms; /* valid while state == TASK_SLEEPING */

  bool is_user;
  uint64_t cr3_phys;
  uint64_t user_entry;
  uint64_t user_stack_top;
  uint64_t user_arg0;
  uint64_t user_arg1;
  uint64_t brk_start; /* just past the highest loaded ELF segment */
  uint64_t brk;       /* current program break, grown by sys_brk */
  struct vfs_file *fds[PROC_MAX_FDS];
  /* This task's own working directory -- real, per-task state now
   * (SYS_chdir/SYS_getcwd, cpu/syscall.c), not the shell-local static
   * the kernel shell used to keep entirely to itself. Always an
   * absolute, normalized path (no trailing slash except for "/"
   * itself) -- see fs/vfs.c's vfs_resolve_relative(), the one place
   * that's allowed to write into this field via a successful chdir.
   * Defaults to "/" at creation (task_create()/task_create_user()) --
   * split()'s child inherits the parent's instead (cpu/syscall.c's
   * sys_split_impl), matching how brk_start/brk/fds are inherited
   * there; an ordinary process_spawn() never inherits one at all
   * (same "no argv/envp, no inherited environment" boundary every
   * other freshly-spawned process already has). */
  char cwd[TASK_CWD_MAX];
  bool waitable;
  int exit_code;
  struct task *waiting_parent;
  bool reaped;
  volatile bool switched_away;
  volatile bool kill_requested;

  struct task *parent;
  bool waiting_for_any_child;
  struct task *next;
  struct task *wq_next;
  struct task *reg_next;
};

struct wait_queue {
  struct task *waiters_head;
  struct task *waiters_tail;
  spinlock_t lock;
};

void sched_init(void);

struct task *task_create(const char *name, task_entry_t entry, void *arg);

/* `uid` is the clearance this task starts with -- see struct
 * task::uid's own comment for who's responsible for choosing it and
 * why there's no default. */
struct task *task_create_user(const char *name, uint64_t cr3_phys,
                              uint64_t entry, uint64_t user_stack_top,
                              uint64_t arg0, uint64_t arg1, uint32_t uid);

void task_publish(struct task *t);

int sched_wait_task(struct task *child);

int64_t sched_wait_any(int *code_out);

struct task *sched_find_waitable_task(uint64_t id);
bool sched_task_is_dead(struct task *t);
void sched_kill_task(struct task *t);

void sched_for_each_task(void (*fn)(struct task *t, void *arg), void *arg);

NORETURN void sched_enter_idle(void);

/* Called from the timer ISR on every tick. */
void sched_tick(void);

/* Voluntarily gives up the remainder of this task's time slice. */
void sched_yield(void);

/* Blocks the calling task for approximately `ms` milliseconds. Must be
 * called from task context (not from an interrupt handler). */
void sched_sleep_ms(uint32_t ms);
void wait_queue_register(struct wait_queue *wq);
void task_block(void);
void wait_queue_block(struct wait_queue *wq);
void wait_queue_wake(struct wait_queue *wq);

/* Ends the calling task. Never returns. */
NORETURN void task_exit(void);

uint32_t sched_task_count(void);

/* Diagnostic snapshot of the struct task slab cache -- see mm/slab.h.
 * `allocated_out` is the number of live tasks currently checked out
 * (should track sched_task_count() closely, modulo the tiny window
 * between a slab_alloc() and this task becoming visible to sched);
 * `pages_out` is how many PMM pages the cache has ever committed
 * (never shrinks, see slab_free()'s own comment on why). Either
 * pointer may be NULL if you only want the other. */
void sched_task_cache_stats(uint64_t *allocated_out, uint64_t *pages_out);

#endif /* NEXUS_SCHED_H */
