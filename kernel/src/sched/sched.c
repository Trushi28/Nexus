#include "sched/sched.h"
#include "cpu/cpu.h"
#include "cpu/gdt.h"
#include "cpu/io.h"
#include "boot/requests.h"
#include "mm/pmm.h"
#include "mm/heap.h"
#include "mm/vmm.h"
#include "fs/vfs.h"
#include "sync/spinlock.h"
#include "time/timer.h"
#include "debug/log.h"
#include "panic.h"

#define KERNEL_STACK_PAGES 4
#define TIME_SLICE_TICKS   10 /* 10ms quantum at TIMER_HZ = 1000 */

extern void context_switch(uint64_t *old_rsp_out, uint64_t new_rsp);

static struct task *ready_head, *ready_tail;
static spinlock_t ready_lock = SPINLOCK_INIT;

static struct task *sleep_head;
static spinlock_t sleep_lock = SPINLOCK_INIT;

static struct task *zombie_head;
static spinlock_t zombie_lock = SPINLOCK_INIT;

/* Guards the exit/wait rendezvous between task_exit() and
 * sched_wait_task() -- see the latter for why this needs to be a real
 * lock rather than the check-then-block idiom used elsewhere in this
 * file (e.g. wait_queue_block()/keyboard_getc()). */
static spinlock_t wait_lock = SPINLOCK_INIT;

/* Every currently-live task, kernel or user, idle or not -- used only
 * by `ps` (sched_for_each_task()). Independent of the ready/sleep/
 * zombie queues above. */
static struct task *registry_head;
static spinlock_t registry_lock = SPINLOCK_INIT;

static uint64_t next_task_id = 1;
static uint32_t live_task_count = 0;

static void push_ready_locked(struct task *t) {
    t->next = NULL;
    if (ready_tail) {
        ready_tail->next = t;
    } else {
        ready_head = t;
    }
    ready_tail = t;
}

static struct task *pop_ready_locked(void) {
    struct task *t = ready_head;
    if (t) {
        ready_head = t->next;
        if (ready_head == NULL) {
            ready_tail = NULL;
        }
        t->next = NULL;
    }
    return t;
}

static void enqueue_ready(struct task *t) {
    uint64_t f = spinlock_acquire_irqsave(&ready_lock);
    push_ready_locked(t);
    spinlock_release_irqrestore(&ready_lock, f);
}

static void register_task(struct task *t) {
    uint64_t f = spinlock_acquire_irqsave(&registry_lock);
    t->reg_next = registry_head;
    registry_head = t;
    spinlock_release_irqrestore(&registry_lock, f);
}

static void unregister_task(struct task *t) {
    uint64_t f = spinlock_acquire_irqsave(&registry_lock);
    struct task **pp = &registry_head;
    while (*pp != NULL && *pp != t) {
        pp = &(*pp)->reg_next;
    }
    if (*pp == t) {
        *pp = t->reg_next;
    }
    spinlock_release_irqrestore(&registry_lock, f);
}

void sched_for_each_task(void (*fn)(struct task *t, void *arg), void *arg) {
    uint64_t f = spinlock_acquire_irqsave(&registry_lock);
    for (struct task *t = registry_head; t != NULL; t = t->reg_next) {
        fn(t, arg);
    }
    spinlock_release_irqrestore(&registry_lock, f);
}

static NORETURN void task_bootstrap(void) {
    /* First-ever run of a freshly created task lands here (see the fake
     * stack frame task_create() builds). Mirrors the sti() that
     * schedule()'s tail would otherwise have done for a resumed task. */
    sti();
    struct task *t = this_cpu()->current_task;
    t->entry(t->arg);
    task_exit();
}

static NORETURN void task_bootstrap_user(void) {
    /* First-ever run of a task_create_user() task. Same idea as
     * task_bootstrap() above, but instead of calling a kernel function
     * pointer we drop to ring 3 and never come back this way -- the
     * task's only route back into the kernel from here on is a trap
     * (syscall, exception, or timer preemption). */
    sti();
    struct task *t = this_cpu()->current_task;

    if (t->cr3_phys != 0) {
        write_cr3(t->cr3_phys);
    }

    uint64_t user_ss     = GDT_USER_DATA; /* already RPL=3 -- see cpu/gdt.h */
    uint64_t user_cs     = GDT_USER_CODE;
    uint64_t user_rflags = 0x202;         /* reserved bit 1 (always 1) + IF */
    uint64_t user_rsp    = t->user_stack_top;
    uint64_t user_rip    = t->user_entry;

    /* IRETQ pops RIP/CS/RFLAGS/RSP/SS off the stack in that order and,
     * because CS's new RPL (3) differs from the current CPL (0), also
     * treats it as a privilege-level change -- exactly what's needed to
     * drop to ring 3. DS/ES/FS/GS are deliberately left completely
     * alone: IRETQ doesn't touch them, 64-bit mode doesn't enforce
     * segment-limit/DPL checks against them for ordinary memory
     * references (so there's nothing to gain by reloading them), and
     * there's a real hazard in doing so -- GS in particular still
     * carries this CPU's GS_BASE-backed cpu_local pointer (this_cpu()
     * reads %gs:0), and reloading the GS *selector* would reset that
     * hidden base to 0 with nothing to restore it until an actual
     * swapgs-based syscall entry exists. Leaving the selector alone
     * sidesteps the issue entirely: GS_BASE was set via wrmsr(), not a
     * segment load, and stays put regardless of CPL. */
    asm volatile (
        "push %0\n\t"
        "push %1\n\t"
        "push %2\n\t"
        "push %3\n\t"
        "push %4\n\t"
        "iretq\n\t"
        :
        : "r"(user_ss), "r"(user_rsp), "r"(user_rflags), "r"(user_cs), "r"(user_rip)
        : "memory"
    );

    __builtin_unreachable();
}

void sched_init(void) {
    ready_head = ready_tail = NULL;
    sleep_head = NULL;
    zombie_head = NULL;
    registry_head = NULL;
}

struct task *task_create(const char *name, task_entry_t entry, void *arg) {
    struct task *t = kzalloc(sizeof(struct task));
    if (t == NULL) {
        panic("sched: out of memory creating task '%s'", name);
    }

    uint64_t stack_phys = pmm_alloc_pages(KERNEL_STACK_PAGES);
    if (stack_phys == 0) {
        panic("sched: out of memory allocating a stack for '%s'", name);
    }

    t->kernel_stack_phys = stack_phys;
    t->kernel_stack_pages = KERNEL_STACK_PAGES;
    t->id = __atomic_fetch_add(&next_task_id, 1, __ATOMIC_RELAXED);
    strncpy(t->name, name, sizeof(t->name) - 1);
    t->entry = entry;
    t->arg = arg;
    t->state = TASK_READY;

    uint64_t stack_top = (uint64_t)phys_to_virt(stack_phys) + KERNEL_STACK_PAGES * PAGE_SIZE;
    uint64_t *sp = (uint64_t *)stack_top;
    *(--sp) = (uint64_t)task_bootstrap; /* "return address" for context_switch's ret */
    *(--sp) = 0; /* rbx */
    *(--sp) = 0; /* rbp */
    *(--sp) = 0; /* r12 */
    *(--sp) = 0; /* r13 */
    *(--sp) = 0; /* r14 */
    *(--sp) = 0; /* r15 */
    t->rsp = (uint64_t)sp;

    __atomic_fetch_add(&live_task_count, 1, __ATOMIC_RELAXED);
    register_task(t);
    enqueue_ready(t);
    return t;
}

struct task *task_create_user(const char *name, uint64_t cr3_phys,
                               uint64_t entry, uint64_t user_stack_top) {
    struct task *t = kzalloc(sizeof(struct task));
    if (t == NULL) {
        kprintf("sched: out of memory creating task '%s'\n", name);
        return NULL;
    }

    uint64_t stack_phys = pmm_alloc_pages(KERNEL_STACK_PAGES);
    if (stack_phys == 0) {
        kprintf("sched: out of memory allocating a kernel stack for '%s'\n", name);
        kfree(t);
        return NULL;
    }

    t->kernel_stack_phys = stack_phys;
    t->kernel_stack_pages = KERNEL_STACK_PAGES;
    t->id = __atomic_fetch_add(&next_task_id, 1, __ATOMIC_RELAXED);
    strncpy(t->name, name, sizeof(t->name) - 1);
    t->state = TASK_READY;
    t->is_user = true;
    t->waitable = true;
    t->cr3_phys = cr3_phys;
    t->user_entry = entry;
    t->user_stack_top = user_stack_top;

    uint64_t stack_top = (uint64_t)phys_to_virt(stack_phys) + KERNEL_STACK_PAGES * PAGE_SIZE;
    uint64_t *sp = (uint64_t *)stack_top;
    *(--sp) = (uint64_t)task_bootstrap_user;
    *(--sp) = 0; /* rbx */
    *(--sp) = 0; /* rbp */
    *(--sp) = 0; /* r12 */
    *(--sp) = 0; /* r13 */
    *(--sp) = 0; /* r14 */
    *(--sp) = 0; /* r15 */
    t->rsp = (uint64_t)sp;

    __atomic_fetch_add(&live_task_count, 1, __ATOMIC_RELAXED);
    register_task(t);
    enqueue_ready(t);
    return t;
}

static void reap_one_zombie(void) {
    uint64_t f = spinlock_acquire_irqsave(&zombie_lock);
    struct task *z = zombie_head;
    if (z != NULL) {
        zombie_head = z->next;
    }
    spinlock_release_irqrestore(&zombie_lock, f);

    if (z != NULL) {
        unregister_task(z);
        pmm_free_pages(z->kernel_stack_phys, z->kernel_stack_pages);
        kfree(z);
        __atomic_fetch_sub(&live_task_count, 1, __ATOMIC_RELAXED);
    }
}

/* Core switch. Must be called with a stable view of `this_cpu()` (i.e.
 * not migrated mid-call -- true for everything running kernel-only
 * cooperative/preemptive tasks with no CPU migration in v1). */
static void schedule(void) {
    uint64_t saved_flags = read_rflags();
    cli();

    reap_one_zombie();

    struct cpu_local *cpu = this_cpu();
    struct task *prev = cpu->current_task;

    uint64_t f = spinlock_acquire_irqsave(&ready_lock);
    struct task *next = pop_ready_locked();
    spinlock_release_irqrestore(&ready_lock, f);

    if (next == NULL) {
        next = cpu->idle_task;
    }

    if (prev->state == TASK_RUNNING) {
        prev->state = TASK_READY;
        if (prev != cpu->idle_task) {
            enqueue_ready(prev);
        }
    }

    next->state = TASK_RUNNING;
    cpu->current_task = next;

    /* Keep RSP0 pointing at this task's own stack, so a future interrupt
     * that hits while we're at CPL0 (the only privilege level Nexus
     * has right now) has somewhere sane to note it came from, and so
     * usermode tasks -- once they exist -- get the right kernel stack
     * from day one. */
    tss_set_rsp0(&cpu->tss, (uint64_t)phys_to_virt(next->kernel_stack_phys) +
                             next->kernel_stack_pages * PAGE_SIZE);

    /* Ring-3 tasks each carry their own address space; kernel tasks
     * (cr3_phys == 0) share the kernel's. Comparing against the
     * currently-loaded CR3 keeps the overwhelmingly common case --
     * kernel task to kernel task -- exactly as cheap as before this
     * existed (one read_cr3(), no write, no TLB flush). */
    uint64_t next_cr3 = (next->cr3_phys != 0) ? next->cr3_phys : vmm_kernel_pml4_phys();
    if (next_cr3 != read_cr3()) {
        write_cr3(next_cr3);
    }

    if (next != prev) {
        context_switch(&prev->rsp, next->rsp);
        /* Control returns here only once `prev` (THIS call site, on
         * whichever task made it) is scheduled back in -- possibly much
         * later, possibly on this same CPU after arbitrarily many other
         * tasks have run. */
    }

    if (saved_flags & (1ULL << 9)) {
        sti();
    }
}

NORETURN void sched_enter_idle(void) {
    struct cpu_local *cpu = this_cpu();

    struct task *idle = kzalloc(sizeof(struct task));
    strncpy(idle->name, cpu->is_bsp ? "idle/bsp" : "idle/ap", sizeof(idle->name) - 1);
    idle->id = 0;
    idle->state = TASK_RUNNING;

    cpu->idle_task = idle;
    cpu->current_task = idle;
    register_task(idle);

    sti();
    for (;;) {
        hlt();
    }
}

void sched_tick(void) {
    struct cpu_local *cpu = this_cpu();
    uint64_t now = timer_uptime_ms();

    /* Wake any sleepers whose time has come. */
    struct task *woken = NULL;
    uint64_t f = spinlock_acquire_irqsave(&sleep_lock);
    struct task **pp = &sleep_head;
    while (*pp != NULL) {
        struct task *t = *pp;
        if (t->wake_time_ms <= now) {
            *pp = t->next;
            t->next = woken;
            woken = t;
        } else {
            pp = &t->next;
        }
    }
    spinlock_release_irqrestore(&sleep_lock, f);

    while (woken != NULL) {
        struct task *t = woken;
        woken = woken->next;
        t->state = TASK_READY;
        enqueue_ready(t);
    }

    cpu->quantum_ticks++;
    if (cpu->quantum_ticks >= TIME_SLICE_TICKS) {
        cpu->quantum_ticks = 0;
        schedule();
    }
}

void sched_yield(void) {
    schedule();
}

void sched_sleep_ms(uint32_t ms) {
    struct task *t = this_cpu()->current_task;
    t->wake_time_ms = timer_uptime_ms() + ms;
    t->state = TASK_SLEEPING;

    uint64_t f = spinlock_acquire_irqsave(&sleep_lock);
    t->next = sleep_head;
    sleep_head = t;
    spinlock_release_irqrestore(&sleep_lock, f);

    schedule();
}

void wait_queue_block(struct wait_queue *wq) {
    struct task *t = this_cpu()->current_task;
    wq->waiter = t;
    t->state = TASK_BLOCKED;
    schedule();
}

void wait_queue_wake(struct wait_queue *wq) {
    struct task *t = wq->waiter;
    if (t == NULL) {
        return;
    }
    wq->waiter = NULL;
    t->state = TASK_READY;
    enqueue_ready(t);
}

NORETURN void task_exit(void) {
    struct task *t = this_cpu()->current_task;

    /* Mark DEAD and check/clear our waiting parent atomically under
     * wait_lock -- see sched_wait_task() for why this can't be the
     * plain check-then-block idiom the rest of this file gets away
     * with (keyboard_getc() et al self-heal from a lost wakeup because
     * their producer fires repeatedly; a process only exits once). */
    uint64_t wf = spinlock_acquire_irqsave(&wait_lock);
    t->state = TASK_DEAD;
    struct task *waiter = t->waiting_parent;
    t->waiting_parent = NULL;
    spinlock_release_irqrestore(&wait_lock, wf);

    if (waiter != NULL) {
        waiter->state = TASK_READY;
        enqueue_ready(waiter);
    }

    if (!t->waitable) {
        /* Nobody will ever sched_wait_task() this one -- it's a plain
         * task_create() kernel task -- so the background reaper owns
         * freeing it, same as always. */
        uint64_t f = spinlock_acquire_irqsave(&zombie_lock);
        t->next = zombie_head;
        zombie_head = t;
        spinlock_release_irqrestore(&zombie_lock, f);
    }
    /* Waitable tasks are instead reaped by sched_wait_task() once its
     * caller has actually collected the exit code -- freeing it here
     * unconditionally would race a parent that hasn't looked yet. It
     * sits inertly (off every queue, so the scheduler will never touch
     * it again) until then. */

    schedule();
    panic("sched: a dead task resumed execution -- this should be impossible");
}

int sched_wait_task(struct task *child) {
    uint64_t f = spinlock_acquire_irqsave(&wait_lock);
    if (child->state != TASK_DEAD) {
        struct task *me = this_cpu()->current_task;
        child->waiting_parent = me;
        me->state = TASK_BLOCKED;
        spinlock_release_irqrestore(&wait_lock, f);
        schedule();
    } else {
        spinlock_release_irqrestore(&wait_lock, f);
    }

    int code = child->exit_code;
    unregister_task(child);

    /* Close whatever the process left open and tear down its address
     * space -- safe now: `child` hasn't been runnable since it called
     * task_exit(), so its cr3_phys is guaranteed not to be the
     * currently loaded CR3 on any CPU (see vmm_free_user_space()'s own
     * comment on why that means no TLB shootdown is needed either). */
    for (int i = 0; i < PROC_MAX_FDS; i++) {
        if (child->fds[i] != NULL) {
            vfs_close(child->fds[i]);
            child->fds[i] = NULL;
        }
    }
    if (child->cr3_phys != 0) {
        vmm_free_user_space(child->cr3_phys);
    }

    pmm_free_pages(child->kernel_stack_phys, child->kernel_stack_pages);
    kfree(child);
    __atomic_fetch_sub(&live_task_count, 1, __ATOMIC_RELAXED);
    return code;
}

uint32_t sched_task_count(void) {
    return __atomic_load_n(&live_task_count, __ATOMIC_RELAXED);
}
