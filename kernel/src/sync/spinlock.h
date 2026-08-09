#ifndef NEXUS_SPINLOCK_H
#define NEXUS_SPINLOCK_H

#include "klib/klib.h"

/* A fair (FIFO) ticket lock. Plain test-and-set locks can starve a CPU
 * indefinitely under contention; on a machine with more than a couple of
 * cores that stops being a theoretical concern, so we pay the (tiny)
 * extra cost for fairness up front. */
typedef struct {
    volatile uint32_t next_ticket;
    volatile uint32_t now_serving;
} spinlock_t;

#define SPINLOCK_INIT { .next_ticket = 0, .now_serving = 0 }

static inline void spinlock_init(spinlock_t *lock) {
    lock->next_ticket = 0;
    lock->now_serving = 0;
}

static inline void spinlock_acquire(spinlock_t *lock) {
    uint32_t my_ticket = __atomic_fetch_add(&lock->next_ticket, 1, __ATOMIC_RELAXED);
    while (__atomic_load_n(&lock->now_serving, __ATOMIC_ACQUIRE) != my_ticket) {
        asm volatile ("pause");
    }
}

static inline void spinlock_release(spinlock_t *lock) {
    uint32_t next = lock->now_serving + 1;
    __atomic_store_n(&lock->now_serving, next, __ATOMIC_RELEASE);
}

/* IRQ-safe variants: disable local interrupts before taking the lock and
 * restore the previous IF state on release. Mandatory for any lock that
 * might also be taken from an interrupt handler, or a handler firing on
 * this CPU while we hold the lock would deadlock against ourselves. */
static inline uint64_t spinlock_acquire_irqsave(spinlock_t *lock) {
    uint64_t flags;
    asm volatile ("pushfq; pop %0" : "=r"(flags));
    asm volatile ("cli");
    spinlock_acquire(lock);
    return flags;
}

static inline void spinlock_release_irqrestore(spinlock_t *lock, uint64_t flags) {
    spinlock_release(lock);
    if (flags & (1 << 9)) { /* RFLAGS.IF */
        asm volatile ("sti");
    }
}

#endif /* NEXUS_SPINLOCK_H */
