#ifndef NEXUS_LAPIC_H
#define NEXUS_LAPIC_H

#include "klib/klib.h"

/* Brings up the Local APIC on the *calling* CPU: prefers x2APIC (pure
 * MSR access, no MMIO mapping needed) when CPUID advertises it, falls
 * back to classic MMIO xAPIC otherwise. Must be called once per CPU,
 * including every AP, after the VMM is up (the xAPIC fallback path needs
 * vmm_map_mmio()). */
void lapic_init(void);

bool lapic_using_x2apic(void);
uint32_t lapic_id(void);
bool lapic_is_ready(void);
void lapic_send_eoi(void);

void lapic_send_ipi(uint32_t dest_apic_id, uint8_t vector);
void lapic_send_ipi_all_excluding_self(uint8_t vector);
void lapic_send_init_ipi(uint32_t dest_apic_id);
void lapic_send_startup_ipi(uint32_t dest_apic_id, uint8_t start_page);

/* Timer. `divide` is the APIC timer's divide-by value encoded per the
 * Intel SDM's Divide Configuration Register table (1=0b1011, 2=0b0000,
 * 4=0b0001, 8=0b0010, 16=0b0011, 32=0b1000, 64=0b1001, 128=0b1010). */
void lapic_timer_stop(void);
void lapic_timer_set_divide(uint8_t divide_encoding);
void lapic_timer_start_oneshot(uint8_t vector, uint32_t initial_count);
void lapic_timer_start_periodic(uint8_t vector, uint32_t initial_count);
/* Masked variant for calibration: counts down like normal but can never
 * actually raise an interrupt, so it's safe to use before any handler
 * for the vector exists or before interrupts are globally enabled. */
void lapic_timer_start_oneshot_masked(uint32_t initial_count);
uint32_t lapic_timer_current_count(void);

#endif /* NEXUS_LAPIC_H */
