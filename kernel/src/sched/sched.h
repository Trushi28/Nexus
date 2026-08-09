#ifndef NEXUS_SCHED_H
#define NEXUS_SCHED_H

#include "klib/klib.h"

enum task_state {
    TASK_READY,
    TASK_RUNNING,
    TASK_SLEEPING,
    TASK_BLOCKED,
    TASK_DEAD,
};

typedef void (*task_entry_t)(void *arg);

struct vfs_file; /* fs/vfs.h -- forward-declared so this header (and
                     everything that includes it) doesn't have to pull
                     in the whole VFS API just to hold fd pointers. */

#define PROC_MAX_FDS 16

struct task {
    uint64_t rsp;             /* only valid while NOT the running task */
    uint64_t kernel_stack_phys;
    size_t   kernel_stack_pages;

    uint64_t id;
    char     name[32];
    enum task_state state;

    task_entry_t entry;
    void        *arg;

    uint64_t wake_time_ms;    /* valid while state == TASK_SLEEPING */

    /* --------------------------- ring-3 state ---------------------------
     * All zero/false for a plain kernel task (the common case, and every
     * task before this patch). Set by task_create_user(), never by
     * task_create(). */
    bool     is_user;
    uint64_t cr3_phys;         /* this task's own address space, or 0 to
                                   run in the kernel's shared one */
    uint64_t user_entry;       /* ring-3 entry point (a user virtual addr) */
    uint64_t user_stack_top;
    uint64_t brk_start;        /* just past the highest loaded ELF segment */
    uint64_t brk;              /* current program break, grown by sys_brk */
    struct vfs_file *fds[PROC_MAX_FDS];

    /* ------------------------ exit / wait rendezvous ---------------------
     * See the race-condition comment on sched_wait_task() in sched.c for
     * why this needs its own locking instead of reusing wait_queue_*(). */
    bool         waitable;     /* true only for task_create_user() tasks --
                                   see task_exit()'s zombie-list comment */
    int          exit_code;
    struct task *waiting_parent;

    struct task *next;         /* intrusive link: ready queue, sleep queue,
                                   wait queue, or zombie list -- never more
                                   than one at a time */
    struct task *reg_next;     /* intrusive link: the global task registry
                                   (see sched_for_each_task()) -- independent
                                   of `next` since a task is *always* in the
                                   registry while also being in at most one
                                   of the queues above */
};

/* A tiny single-owner wait queue: enough for the shell to block on
 * keyboard input without busy-polling. Not a general condvar -- see
 * sched_wait_task() for why process exit needs a properly-locked
 * rendezvous instead of this. */
struct wait_queue {
    struct task *waiter;
};

void sched_init(void);

/* Allocates a kernel stack and queues a new ready task. Safe to call
 * before the scheduler is actually driving anything (tasks just sit
 * ready until the BSP hands control to the scheduler). */
struct task *task_create(const char *name, task_entry_t entry, void *arg);

/* Like task_create(), but for a ring-3 process: `entry` and
 * `user_stack_top` are *user* virtual addresses in the address space
 * named by `cr3_phys` (see vmm_new_address_space() + elf_load()). The
 * task's first run drops straight to ring 3 via IRETQ instead of
 * calling a kernel function pointer. Returns NULL (having already
 * logged why) on OOM rather than panicking -- a failed `run` shouldn't
 * take the whole kernel down. */
struct task *task_create_user(const char *name, uint64_t cr3_phys,
                               uint64_t entry, uint64_t user_stack_top);

/* Blocks the calling task until `child` (a task_create_user() task)
 * exits, then reaps it and returns its exit code. Call at most once
 * per child -- the second call would touch a freed struct task. */
int sched_wait_task(struct task *child);

/* Calls `fn(task, arg)` once for every currently-live task (used by
 * `ps`). Runs with the registry lock held, so `fn` must be quick and
 * must not itself create or reap tasks. */
void sched_for_each_task(void (*fn)(struct task *t, void *arg), void *arg);

/* Converts the calling context (whatever it is -- kmain()'s own boot
 * stack, or an AP's post-init idle loop) into that CPU's idle task, and
 * enters an infinite idle loop. Only returns if something has gone
 * badly wrong. Call once, at the very end of init, on every CPU. */
NORETURN void sched_enter_idle(void);

/* Called from the timer ISR on every tick. */
void sched_tick(void);

/* Voluntarily gives up the remainder of this task's time slice. */
void sched_yield(void);

/* Blocks the calling task for approximately `ms` milliseconds. Must be
 * called from task context (not from an interrupt handler). */
void sched_sleep_ms(uint32_t ms);

void wait_queue_block(struct wait_queue *wq);
void wait_queue_wake(struct wait_queue *wq);

/* Ends the calling task. Never returns. */
NORETURN void task_exit(void);

uint32_t sched_task_count(void);

#endif /* NEXUS_SCHED_H */
