#ifndef NEXUS_SCHED_H
#define NEXUS_SCHED_H

#include "klib/klib.h"

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

struct task {
  uint64_t rsp; /* only valid while NOT the running task */
  uint64_t kernel_stack_phys;
  size_t kernel_stack_pages;

  uint64_t id;
  char name[32];
  enum task_state state;

  task_entry_t entry;
  void *arg;

  uint64_t wake_time_ms; /* valid while state == TASK_SLEEPING */

  bool is_user;
  uint64_t cr3_phys;
  uint64_t user_entry;
  uint64_t user_stack_top;
  uint64_t brk_start; /* just past the highest loaded ELF segment */
  uint64_t brk;       /* current program break, grown by sys_brk */
  struct vfs_file *fds[PROC_MAX_FDS];
  bool waitable;
  int exit_code;
  struct task *waiting_parent;
  volatile bool switched_away;
  struct task *next;
  struct task *reg_next;
};
struct wait_queue {
  struct task *waiter;
};

void sched_init(void);

struct task *task_create(const char *name, task_entry_t entry, void *arg);

struct task *task_create_user(const char *name, uint64_t cr3_phys,
                              uint64_t entry, uint64_t user_stack_top);

int sched_wait_task(struct task *child);
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

#endif /* NEXUS_SCHED_H */
