#ifndef NEXUS_PIT_H
#define NEXUS_PIT_H

#include "klib/klib.h"

/* Busy-waits for approximately `ms` milliseconds (max ~54ms per call --
 * that's as long as the 16-bit PIT counter can represent at its native
 * 1.193182 MHz) using channel 0 in rate-generator mode, polled, no
 * interrupts involved. Used exactly once at boot, to calibrate the LAPIC
 * timer against a known-good clock; nothing else in the kernel depends
 * on the PIT. */
void pit_wait_ms(uint32_t ms);

#endif /* NEXUS_PIT_H */
