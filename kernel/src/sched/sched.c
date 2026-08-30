#include "sched/sched.h"
#include "boot/requests.h"
#include "cpu/cpu.h"
#include "cpu/gdt.h"
#include "cpu/io.h"
#include "debug/log.h"
#include "fs/vfs.h"
#include "mm/pmm.h"
#include "mm/slab.h"
#include "mm/vmm.h"
#include "panic.h"
#include "sync/spinlock.h"
#include "time/timer.h"

#define KERNEL_STACK_PAGES 4
#define TIME_SLICE_TICKS 10 // 10ms quantum at TIMER_HZ = 1000

extern void context_switch(uint64_t *old_rsp_out, uint64_t new_rsp,
                           volatile bool *old_switched_away_out);
static void wake_blocked_task(struct task *t);

static struct task *ready_head, *ready_tail;
static spinlock_t ready_lock = SPINLOCK_INIT;

static struct task *sleep_head;
static spinlock_t sleep_lock = SPINLOCK_INIT;

/* Guards the exit/wait rendezvous, plus struct task::parent/
 * waiting_for_any_child. registry_lock is always acquired INSIDE a
 * wait_lock hold, never the other order -- keep that direction if a
 * third lock ever needs both. */
static spinlock_t wait_lock = SPINLOCK_INIT;

// Every live task, kernel or user -- independent of the ready/sleep/parked queues.
static struct task *registry_head;
static spinlock_t registry_lock = SPINLOCK_INIT;

static uint64_t next_task_id = 1;
static uint32_t live_task_count = 0;

static struct slab_cache task_cache;

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

/* Clears `parent` on any live task pointing at `dying`, whose struct is
 * about to be freed -- held under wait_lock so no reader can observe
 * a stale, now-dangling pointer (see struct task::parent). */
static void orphan_children(struct task *dying) {
  uint64_t wf = spinlock_acquire_irqsave(&wait_lock);
  uint64_t rf = spinlock_acquire_irqsave(&registry_lock);
  for (struct task *t = registry_head; t != NULL; t = t->reg_next) {
    if (t->parent == dying) {
      t->parent = NULL;
    }
  }
  spinlock_release_irqrestore(&registry_lock, rf);
  spinlock_release_irqrestore(&wait_lock, wf);
}

struct task *sched_find_waitable_task(uint64_t id) {
  uint64_t f = spinlock_acquire_irqsave(&registry_lock);
  struct task *result = NULL;
  for (struct task *t = registry_head; t != NULL; t = t->reg_next) {
    if (t->id == id) {
      if (t->waitable) {
        result = t;
      }
      break;
    }
  }
  spinlock_release_irqrestore(&registry_lock, f);
  return result;
}

bool sched_task_is_dead(struct task *t) {
  return __atomic_load_n(&t->state, __ATOMIC_ACQUIRE) == TASK_DEAD;
}

void sched_kill_task(struct task *t) {
  __atomic_store_n(&t->kill_requested, true, __ATOMIC_RELEASE);
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
  uint64_t user_rdi = t->user_arg0;
  uint64_t user_rsi = t->user_arg1;

  asm volatile("push %0\n\t"
               "push %1\n\t"
               "push %2\n\t"
               "push %3\n\t"
               "push %4\n\t"
               "iretq\n\t"
               :
               : "r"(user_ss), "r"(user_rsp), "r"(user_rflags), "r"(user_cs),
                 "r"(user_rip), "D"(user_rdi), "S"(user_rsi)
               : "memory");

  __builtin_unreachable();
}

void sched_init(void) {
  ready_head = ready_tail = NULL;
  sleep_head = NULL;
  registry_head = NULL;
  slab_cache_init(&task_cache, sizeof(struct task), "task");
}

struct task *task_create(const char *name, task_entry_t entry, void *arg) {
  struct task *t = slab_alloc(&task_cache);
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
  t->uid = 0;
  t->cwd[0] = '/';
  t->cwd[1] = '\0';
  uint64_t stack_top =
      (uint64_t)phys_to_virt(stack_phys) + KERNEL_STACK_PAGES * PAGE_SIZE;
  uint64_t *sp = (uint64_t *)stack_top;
  *(--sp) = (uint64_t)task_bootstrap;
  *(--sp) = 0; // rbx
  *(--sp) = 0; // rbp
  *(--sp) = 0; // r12
  *(--sp) = 0; // r13
  *(--sp) = 0; // r14
  *(--sp) = 0; // r15
  t->rsp = (uint64_t)sp;

  __atomic_fetch_add(&live_task_count, 1, __ATOMIC_RELAXED);
  register_task(t);
  enqueue_ready(t);
  return t;
}

struct task *task_create_user(const char *name, uint64_t cr3_phys,
                              uint64_t entry, uint64_t user_stack_top,
                              uint64_t arg0, uint64_t arg1, uint32_t uid) {
  struct task *t = slab_alloc(&task_cache);
  if (t == NULL) {
    kprintf("sched: out of memory creating task '%s'\n", name);
    return NULL;
  }

  uint64_t stack_phys = pmm_alloc_pages(KERNEL_STACK_PAGES);
  if (stack_phys == 0) {
    kprintf("sched: out of memory allocating a kernel stack for '%s'\n", name);
    slab_free(&task_cache, t);
    return NULL;
  }

  t->kernel_stack_phys = stack_phys;
  t->kernel_stack_pages = KERNEL_STACK_PAGES;
  t->id = __atomic_fetch_add(&next_task_id, 1, __ATOMIC_RELAXED);
  strncpy(t->name, name, sizeof(t->name) - 1);
  t->state = TASK_READY;
  t->is_user = true;
  t->waitable = true;
  t->uid = uid;
  t->cwd[0] = '/';
  t->cwd[1] = '\0';
  t->cr3_phys = cr3_phys;
  t->user_entry = entry;
  t->user_stack_top = user_stack_top;
  t->user_arg0 = arg0;
  t->user_arg1 = arg1;

  uint64_t stack_top =
      (uint64_t)phys_to_virt(stack_phys) + KERNEL_STACK_PAGES * PAGE_SIZE;
  uint64_t *sp = (uint64_t *)stack_top;
  *(--sp) = (uint64_t)task_bootstrap_user;
  *(--sp) = 0; // rbx
  *(--sp) = 0; // rbp
  *(--sp) = 0; // r12
  *(--sp) = 0; // r13
  *(--sp) = 0; // r14
  *(--sp) = 0; // r15
  t->rsp = (uint64_t)sp;

  __atomic_fetch_add(&live_task_count, 1, __ATOMIC_RELAXED);
  register_task(t);
  // Deliberately NOT enqueue_ready() -- see task_publish().
  return t;
}

void task_publish(struct task *t) { enqueue_ready(t); }

static void finish_task_exit(struct task *t) {
  uint64_t wf = spinlock_acquire_irqsave(&wait_lock);
  t->state = TASK_DEAD;
  struct task *waiter = t->waiting_parent;
  t->waiting_parent = NULL;

  struct task *any_parent = t->parent;
  bool wake_any = (any_parent != NULL && any_parent->waiting_for_any_child);
  spinlock_release_irqrestore(&wait_lock, wf);

  if (waiter != NULL) {
    wake_blocked_task(waiter);
  }
  if (wake_any) {
    wake_blocked_task(any_parent);
  }

  if (!t->waitable) {
    unregister_task(t);
    orphan_children(t);
    pmm_free_pages(t->kernel_stack_phys, t->kernel_stack_pages);
    slab_free(&task_cache, t);
    __atomic_fetch_sub(&live_task_count, 1, __ATOMIC_RELAXED);
  }
}

/* Flip state only -- never touch parked_head or enqueue_ready() here.
 * Only the owning cpu's own schedule() drain may publish a parked task. */
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

  /* Drain: every parked_head entry was parked by THIS cpu's own past
   * schedule() call, so its context_switch() away is guaranteed done. */
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
      // Park locally -- prev->rsp isn't valid until context_switch() below runs.
      prev->next = cpu->parked_head;
      cpu->parked_head = prev;
    }
  } else if (prev->state == TASK_EXITING) {
    cpu->pending_exit = prev;
  } else if (prev->state == TASK_BLOCKED || prev->state == TASK_SLEEPING) {
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

  struct task *idle = slab_alloc(&task_cache);
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
  schedule();
}

void wait_queue_register(struct wait_queue *wq) {
  struct task *t = this_cpu()->current_task;
  t->switched_away = false;
  t->wq_next = NULL;

  uint64_t f = spinlock_acquire_irqsave(&wq->lock);
  if (wq->waiters_tail != NULL) {
    wq->waiters_tail->wq_next = t;
  } else {
    wq->waiters_head = t;
  }
  wq->waiters_tail = t;
  spinlock_release_irqrestore(&wq->lock, f);
}

void task_block(void) {
  struct task *t = this_cpu()->current_task;
  enum task_state expected = TASK_RUNNING;
  if (__atomic_compare_exchange_n(&t->state, &expected, TASK_BLOCKED, false,
                                  __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
    schedule();
    return;
  }
  // CAS failed -- a wake already landed; nothing to block on.
}

void wait_queue_block(struct wait_queue *wq) {
  wait_queue_register(wq);
  task_block();
}

void wait_queue_wake(struct wait_queue *wq) {
  uint64_t f = spinlock_acquire_irqsave(&wq->lock);
  struct task *t = wq->waiters_head;
  if (t != NULL) {
    wq->waiters_head = t->wq_next;
    if (wq->waiters_head == NULL) {
      wq->waiters_tail = NULL;
    }
    t->wq_next = NULL;
  }
  spinlock_release_irqrestore(&wq->lock, f);

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
  if (child->reaped) {
    spinlock_release_irqrestore(&wait_lock, f);
    return -1;
  }
  if (child->state != TASK_DEAD) {
    if (child->waiting_parent != NULL) {
      spinlock_release_irqrestore(&wait_lock, f);
      return -1;
    }
    struct task *me = this_cpu()->current_task;
    me->switched_away = false;
    child->waiting_parent = me;
    me->state = TASK_BLOCKED;
    spinlock_release_irqrestore(&wait_lock, f);
    schedule();
    f = spinlock_acquire_irqsave(&wait_lock);
    if (child->reaped) {
      spinlock_release_irqrestore(&wait_lock, f);
      return -1;
    }
  }
  child->reaped = true;
  spinlock_release_irqrestore(&wait_lock, f);

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
  orphan_children(child);

  pmm_free_pages(child->kernel_stack_phys, child->kernel_stack_pages);
  slab_free(&task_cache, child);
  __atomic_fetch_sub(&live_task_count, 1, __ATOMIC_RELAXED);
  return code;
}

int64_t sched_wait_any(int *code_out) {
  struct task *me = this_cpu()->current_task;

  for (;;) {
    uint64_t wf = spinlock_acquire_irqsave(&wait_lock);

    struct task *dead = NULL;
    bool any_child = false;

    uint64_t rf = spinlock_acquire_irqsave(&registry_lock);
    for (struct task *t = registry_head; t != NULL; t = t->reg_next) {
      if (t->parent != me || t->reaped) {
        continue;
      }
      any_child = true;
      if (t->state == TASK_DEAD) {
        dead = t;
        break;
      }
    }
    spinlock_release_irqrestore(&registry_lock, rf);

    if (dead != NULL) {
      dead->reaped = true;
      spinlock_release_irqrestore(&wait_lock, wf);

      int code = dead->exit_code;
      uint64_t pid = dead->id;

      unregister_task(dead);
      for (int i = 0; i < PROC_MAX_FDS; i++) {
        if (dead->fds[i] != NULL) {
          vfs_close(dead->fds[i]);
          dead->fds[i] = NULL;
        }
      }
      if (dead->cr3_phys != 0) {
        vmm_free_user_space(dead->cr3_phys);
      }
      orphan_children(dead);
      pmm_free_pages(dead->kernel_stack_phys, dead->kernel_stack_pages);
      slab_free(&task_cache, dead);
      __atomic_fetch_sub(&live_task_count, 1, __ATOMIC_RELAXED);

      if (code_out != NULL) {
        *code_out = code;
      }
      return (int64_t)pid;
    }

    if (!any_child) {
      spinlock_release_irqrestore(&wait_lock, wf);
      return -1;
    }

    me->switched_away = false;
    me->waiting_for_any_child = true;
    me->state = TASK_BLOCKED;
    spinlock_release_irqrestore(&wait_lock, wf);

    schedule();

    uint64_t wf2 = spinlock_acquire_irqsave(&wait_lock);
    me->waiting_for_any_child = false;
    spinlock_release_irqrestore(&wait_lock, wf2);
  }
}

uint32_t sched_task_count(void) {
  return __atomic_load_n(&live_task_count, __ATOMIC_RELAXED);
}

void sched_task_cache_stats(uint64_t *allocated_out, uint64_t *pages_out) {
  if (allocated_out != NULL) {
    *allocated_out = task_cache.num_allocated;
  }
  if (pages_out != NULL) {
    *pages_out = task_cache.num_pages;
  }
}
