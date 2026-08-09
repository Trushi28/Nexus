#ifndef NEXUS_SMP_H
#define NEXUS_SMP_H

#include "klib/klib.h"
#include "cpu/cpu.h"

/* Registers a cpu_local for the BSP itself (index 0). Call once, very
 * early -- before smp_init() -- once the PMM is up. */
void smp_register_bsp(struct cpu_local *bsp);

/* Releases every AP reported by Limine's SMP feature (skipping the BSP)
 * to run ap_entry(). Fire-and-forget: does not block waiting for them to
 * finish booting -- smp_online_cpu_count() reflects however many have
 * checked in whenever it's queried. Call once, on the BSP, after every
 * BSP-only subsystem (GDT/IDT/PMM/VMM/ACPI/heap/timer calibration) is
 * already up, since every AP immediately depends on all of it. */
void smp_init(void);

uint32_t smp_online_cpu_count(void);

#endif /* NEXUS_SMP_H */
