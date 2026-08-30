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
  uint64_t rsp; // only valid while NOT the running task
  uint64_t kernel_stack_phys;
  size_t kernel_stack_pages;

  uint64_t id;
  uint32_t uid; // this task's clearance -- 0 is root/unrestricted
  char name[32];
  enum task_state state;

  task_entry_t entry;
  void *arg;

  uint64_t wake_time_ms; // valid while state == TASK_SLEEPING

  bool is_user;
  uint64_t cr3_phys;
  uint64_t user_entry;
  uint64_t user_stack_top;
  uint64_t user_arg0;
  uint64_t user_arg1;
  uint64_t brk_start; // just past the highest loaded ELF segment
  uint64_t brk;       // current program break, grown by sys_brk
  struct vfs_file *fds[PROC_MAX_FDS];
  char cwd[TASK_CWD_MAX]; // always absolute/normalized -- see vfs_resolve_relative()
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

void sched_tick(void);

void sched_yield(void);

void sched_sleep_ms(uint32_t ms);
void wait_queue_register(struct wait_queue *wq);
void task_block(void);
void wait_queue_block(struct wait_queue *wq);
void wait_queue_wake(struct wait_queue *wq);

NORETURN void task_exit(void);

uint32_t sched_task_count(void);

// Diagnostic slab-cache snapshot for `ps` -- either pointer may be NULL.
void sched_task_cache_stats(uint64_t *allocated_out, uint64_t *pages_out);

#endif /* NEXUS_SCHED_H */
