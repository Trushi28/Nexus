#ifndef NEXUS_IOAPIC_H
#define NEXUS_IOAPIC_H

#include "klib/klib.h"

/* Maps every I/O APIC found in the MADT (or falls back to the
 * architectural default single I/O APIC at 0xFEC00000 if ACPI parsing
 * came up empty). Call after acpi_init() and after the VMM is up. */
void ioapic_init(void);

/* Routes ISA IRQ `isa_irq` (0-15) to `vector`, delivered to the CPU
 * whose Local APIC ID is `dest_apic_id`. Honours any Interrupt Source
 * Override (polarity/trigger mode/remapped GSI) found in the MADT. */
void ioapic_set_irq(uint8_t isa_irq, uint8_t vector, uint32_t dest_apic_id);

void ioapic_mask_irq(uint8_t isa_irq);

#endif /* NEXUS_IOAPIC_H */
