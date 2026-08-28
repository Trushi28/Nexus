#ifndef NEXUS_PIT_H
#define NEXUS_PIT_H

#include "klib/klib.h"

/*
 * Busy-waits using PIT channel 0 in polled rate-generator mode.
 * Limited to ~54 ms per call by the 16-bit PIT counter. Used during
 * early boot to calibrate the LAPIC timer; the PIT is not otherwise used.
 */
void pit_wait_ms(uint32_t ms);

#endif /* NEXUS_PIT_H */
