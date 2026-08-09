#ifndef NEXUS_TIMER_H
#define NEXUS_TIMER_H

#include "klib/klib.h"

#define TIMER_HZ 1000 /* 1ms scheduler tick */

/* Measures how fast the (per-CPU-identical) LAPIC timer counts against
 * the PIT, a known-good reference clock. Call once, on the BSP, before
 * any CPU starts its periodic timer. Registers the tick ISR too. */
void timer_calibrate(void);

/* Starts this CPU's own LAPIC timer ticking at TIMER_HZ. Call on every
 * CPU (BSP and each AP) after timer_calibrate() has run once. */
void timer_start_periodic_for_this_cpu(void);

/* Milliseconds since timer_calibrate() was called (BSP tick count is
 * used as the wall clock; every CPU's LAPIC timer runs at the same
 * calibrated rate, so this is consistent kernel-wide). */
uint64_t timer_uptime_ms(void);

/* Busy-wait (no scheduling, just spins) -- fine for short boot-time
 * delays; anything a task might want to block on should go through
 * sched_sleep_ms() instead so other work can run meanwhile. */
void timer_busy_wait_ms(uint32_t ms);

#endif /* NEXUS_TIMER_H */
