#ifndef NEXUS_TIMER_H
#define NEXUS_TIMER_H

#include "klib/klib.h"

#define TIMER_HZ 1000 /* 1ms scheduler tick */

/*
 * Calibrates the LAPIC timer against the PIT and registers the tick ISR.
 * Call once on the BSP before any CPU starts its periodic timer.
 */
void timer_calibrate(void);

/* Starts this CPU's own LAPIC timer ticking at TIMER_HZ. Call on every
 * CPU (BSP and each AP) after timer_calibrate() has run once. */
void timer_start_periodic_for_this_cpu(void);

// Returns milliseconds since timer_calibrate().
uint64_t timer_uptime_ms(void);

/*
 * Busy-waits for `ms` milliseconds without scheduling.
 * Use sched_sleep_ms() instead when the caller may sleep.
 */
void timer_busy_wait_ms(uint32_t ms);

#endif /* NEXUS_TIMER_H */
