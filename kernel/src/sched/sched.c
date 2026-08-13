#include "sched/sched.h"
#include "boot/requests.h"
#include "cpu/cpu.h"
#include "cpu/gdt.h"
#include "cpu/io.h"
#include "debug/log.h"
#include "fs/vfs.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "panic.h"
#include "sync/spinlock.h"
#include "time/timer.h"

#define KERNEL_STACK_PAGES 4
#define TIME_SLICE_TICKS 10 /* 10ms quantum at TIMER_HZ = 1000 */

extern void context_switch(uint64_t *old_rsp_out, uint64_t new_rsp,
                           volatile bool *old_switched_away_out);
static void wake_blocked_task(struct task *t);

static struct task *ready_head, *ready_tail;
static spinlock_t ready_lock = SPINLOCK_INIT;

static struct task *sleep_head;
static spinlock_t sleep_lock = SPINLOCK_INIT;

/* Guards the exit/wait rendezvous between task_exit() and
 * sched_wait_task() -- see the latter for why this needs to be a real
 * lock rather than the check-then-block idiom used elsewhere in this
 * file (e.g. wait_queue_block()/keyboard_getc()). */
static spinlock_t wait_lock = SPINLOCK_INIT;

/* Every currently-live task, kernel or user, idle or not -- used only
 * by `ps` (sched_for_each_task()). Independent of the ready/sleep/
 * parked queues -- a task is always in the registry while also being
 * in at most one of those. */
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
  sti();
  struct task *t = this_cpu()->current_task;
  t->entry(t->arg);
  task_exit();
}

static NORETURN void task_bootstrap_user(void) {
  sti();
  struct task *t = this_cpu()->current_task;

  if (t->cr3_phys != 0) {
    write_cr3(t->cr3_phys);
  }

  uint64_t user_ss = GDT_USER_DATA;
  uint64_t user_cs = GDT_USER_CODE;
  uint64_t user_rflags = 0x202;
  uint64_t user_rsp = t->user_stack_top;
  uint64_t user_rip = t->user_entry;

  asm volatile("push %0\n\t"
               "push %1\n\t"
               "push %2\n\t"
               "push %3\n\t"
               "push %4\n\t"
               "iretq\n\t"
               :
               : "r"(user_ss), "r"(user_rsp), "r"(user_rflags), "r"(user_cs),
                 "r"(user_rip)
               : "memory");

  __builtin_unreachable();
}

void sched_init(void) {
  ready_head = ready_tail = NULL;
  sleep_head = NULL;
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

  uint64_t stack_top =
      (uint64_t)phys_to_virt(stack_phys) + KERNEL_STACK_PAGES * PAGE_SIZE;
  uint64_t *sp = (uint64_t *)stack_top;
  *(--sp) = (uint64_t)task_bootstrap;
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

  uint64_t stack_top =
      (uint64_t)phys_to_virt(stack_phys) + KERNEL_STACK_PAGES * PAGE_SIZE;
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

static void finish_task_exit(struct task *t) {
  uint64_t wf = spinlock_acquire_irqsave(&wait_lock);
  t->state = TASK_DEAD;
  struct task *waiter = t->waiting_parent;
  t->waiting_parent = NULL;
  spinlock_release_irqrestore(&wait_lock, wf);

  if (waiter != NULL) {
    wake_blocked_task(waiter);
  }

  if (!t->waitable) {
    unregister_task(t);
    pmm_free_pages(t->kernel_stack_phys, t->kernel_stack_pages);
    kfree(t);
    __atomic_fetch_sub(&live_task_count, 1, __ATOMIC_RELAXED);
  }
}

/* Makes a parked task (TASK_BLOCKED or TASK_SLEEPING) eligible to run
 * again. This is the ONLY thing any caller -- wait_queue_wake(),
 * finish_task_exit()'s parent-wake, anything else that might exist
 * later -- is allowed to do to a task it doesn't own: flip its state.
 * NEVER touch cpu_local::parked_head, and NEVER call enqueue_ready()
 * here, no matter how safe that looks for a specific caller.
 *
 * An earlier version of this function had a "fast path": if the
 * target's switched_away was already true, enqueue_ready() it right
 * here, on the spot, skipping the wait. That was wrong. switched_away
 * being true only proves the OWNING cpu's context_switch() away from
 * this task has completed -- it says nothing about whether that same
 * cpu's drain loop (schedule(), below) has gotten around to actually
 * unlinking the task from its own parked_head list yet. The fast path
 * could enqueue_ready() a task that was still physically linked into
 * parked_head -- and enqueue_ready() overwrites task->next as part of
 * splicing onto the real ready queue, which is the SAME field
 * parked_head's chain was using. The owning cpu's drain loop would
 * then read a stale/overwritten task->next while trying to unlink the
 * very same task, corrupting whichever list it touched, and could
 * enqueue the same task struct a second time -- up to and including
 * self-looping the ready queue.
 *
 * The fix: exactly one writer per list. parked_head is only ever
 * touched by its owning cpu, inside its own schedule() call, both to
 * add an entry (parking) and remove one (draining). Every external
 * actor only ever sets state; the owning cpu's own next schedule()
 * call is what turns that into a real, globally-visible enqueue. The
 * cost is bounded, honest latency -- at most one quantum (~10ms),
 * since sched_tick() re-enters schedule() on every cpu regularly
 * regardless of what's currently running there, idle included -- in
 * exchange for a mechanism that can't corrupt itself under any
 * interleaving. */
static void wake_blocked_task(struct task *t) {
  __atomic_store_n(&t->state, TASK_READY, __ATOMIC_RELEASE);
}

static void schedule(void) {
  uint64_t saved_flags = read_rflags();
  cli();

  struct cpu_local *cpu = this_cpu();

  if (cpu->pending_exit != NULL) {
    finish_task_exit(cpu->pending_exit);
    cpu->pending_exit = NULL;
  }

  /* Drain: every entry here is something THIS cpu parked in a past
   * episode of ITS OWN schedule() -- see the TASK_RUNNING/
   * TASK_BLOCKED/TASK_SLEEPING handling below. Reaching the top of
   * schedule() on this cpu again at all proves every one of those
   * entries' context_switch() away already completed (that switch
   * was the last thing the episode that parked them did before
   * returning here), so switched_away is guaranteed true for all of
   * them -- no separate check needed, only the state each one was
   * parked with (or has since had set by wake_blocked_task()):
   *   - TASK_READY:    safe to publish to the real ready queue now.
   *   - TASK_SLEEPING: safe to publish to the global sleep_head now
   *                    -- sched_tick()'s existing wake-on-timeout
   *                    logic takes it from there, on whichever cpu's
   *                    timer next notices wake_time_ms has passed.
   *   - anything else (still TASK_BLOCKED): not ready yet, leave it
   *                    parked and check again next time.
   * This is the ONLY place parked_head is ever read, written, or
   * drained -- see wake_blocked_task()'s comment for why that's load
   * bearing. */
  {
    struct task **pp = &cpu->parked_head;
    while (*pp != NULL) {
      struct task *t = *pp;
      enum task_state st = __atomic_load_n(&t->state, __ATOMIC_ACQUIRE);
      if (st == TASK_READY) {
        *pp = t->next;
        t->next = NULL;
        enqueue_ready(t);
      } else if (st == TASK_SLEEPING) {
        *pp = t->next;
        uint64_t sf = spinlock_acquire_irqsave(&sleep_lock);
        t->next = sleep_head;
        sleep_head = t;
        spinlock_release_irqrestore(&sleep_lock, sf);
      } else {
        pp = &t->next;
      }
    }
  }

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
      /* Do NOT enqueue_ready(prev) directly -- prev->rsp isn't a
       * valid resume point until context_switch() below actually
       * runs. Park it locally; this cpu's OWN drain loop above,
       * next time schedule() runs here, is what publishes it for
       * real, once that's provably safe. Identical hazard,
       * identical fix, as the TASK_BLOCKED case just below --
       * see wake_blocked_task()'s comment for the full story.
       * This is the hottest path in the whole scheduler (every
       * preempted task, every quantum), and until now was the
       * one case that was never guarded this way. */
      prev->next = cpu->parked_head;
      cpu->parked_head = prev;
    }
  } else if (prev->state == TASK_EXITING) {
    cpu->pending_exit = prev;
  } else if (prev->state == TASK_BLOCKED || prev->state == TASK_SLEEPING) {
    /* TASK_SLEEPING: sched_sleep_ms() sets the state and wake_time_ms
     * and calls schedule() -- it does NOT touch sleep_head itself
     * (it used to; that had the exact same cross-cpu hazard the
     * comment above describes, just one level up: another cpu's
     * sched_tick() could pop this task back off sleep_head and
     * enqueue_ready() it before context_switch() below had run).
     * Parking it here, like TASK_BLOCKED, and only publishing to
     * the global sleep_head from the drain loop once that's safe,
     * closes that the same way. */
    prev->next = cpu->parked_head;
    cpu->parked_head = prev;
  }

  next->state = TASK_RUNNING;
  cpu->current_task = next;

  tss_set_rsp0(&cpu->tss, (uint64_t)phys_to_virt(next->kernel_stack_phys) +
                              next->kernel_stack_pages * PAGE_SIZE);

  uint64_t next_cr3 =
      (next->cr3_phys != 0) ? next->cr3_phys : vmm_kernel_pml4_phys();
  if (next_cr3 != read_cr3()) {
    write_cr3(next_cr3);
  }

  if (next != prev) {
    context_switch(&prev->rsp, next->rsp, &prev->switched_away);
  }

  if (saved_flags & (1ULL << 9)) {
    sti();
  }
}

NORETURN void sched_enter_idle(void) {
  struct cpu_local *cpu = this_cpu();

  struct task *idle = kzalloc(sizeof(struct task));
  strncpy(idle->name, cpu->is_bsp ? "idle/bsp" : "idle/ap",
          sizeof(idle->name) - 1);
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

  /* Wake any sleepers whose time has come. Safe to enqueue_ready()
   * directly here, on whichever cpu's timer happens to notice --
   * unlike the OLD sched_sleep_ms(), a task only ever appears in
   * this list via schedule()'s drain loop, which only publishes it
   * once its owning cpu's context_switch() away has already
   * completed. See schedule()'s comment. */
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

void sched_yield(void) { schedule(); }

void sched_sleep_ms(uint32_t ms) {
  struct task *t = this_cpu()->current_task;
  t->wake_time_ms = timer_uptime_ms() + ms;
  t->state = TASK_SLEEPING;
  /* Does NOT touch sleep_head here -- see schedule()'s TASK_SLEEPING
   * handling for why that has to wait until this cpu has provably
   * finished switching away. */
  schedule();
}

void wait_queue_register(struct wait_queue *wq) {
  struct task *t = this_cpu()->current_task;
  t->switched_away = false;
  __atomic_store_n(&wq->waiter, t, __ATOMIC_RELEASE);
}

void task_block(void) {
  struct task *t = this_cpu()->current_task;
  enum task_state expected = TASK_RUNNING;
  if (__atomic_compare_exchange_n(&t->state, &expected, TASK_BLOCKED, false,
                                  __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
    schedule();
    return;
  }
  /* CAS failed -- a wake already landed before we got this far.
   * Already "woken"; just return instead of blocking. */
}

void wait_queue_block(struct wait_queue *wq) {
  wait_queue_register(wq);
  task_block();
}

void wait_queue_wake(struct wait_queue *wq) {
  struct task *t = __atomic_exchange_n(&wq->waiter, NULL, __ATOMIC_ACQ_REL);
  if (t != NULL) {
    wake_blocked_task(t);
  }
}

NORETURN void task_exit(void) {
  struct task *t = this_cpu()->current_task;
  t->state = TASK_EXITING;
  schedule();
  panic("sched: a dead task resumed execution -- this should be impossible");
}

int sched_wait_task(struct task *child) {
  uint64_t f = spinlock_acquire_irqsave(&wait_lock);
  if (child->state != TASK_DEAD) {
    struct task *me = this_cpu()->current_task;
    me->switched_away = false;
    child->waiting_parent = me;
    me->state = TASK_BLOCKED;
    spinlock_release_irqrestore(&wait_lock, f);
    schedule();
  } else {
    spinlock_release_irqrestore(&wait_lock, f);
  }

  int code = child->exit_code;
  unregister_task(child);

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
