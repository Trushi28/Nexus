#ifndef NEXUS_ACPI_H
#define NEXUS_ACPI_H

#include "klib/klib.h"

#define MAX_IOAPICS 8
#define MAX_ISOS    32

struct acpi_ioapic {
    uint8_t  id;
    uint32_t phys_addr;
    uint32_t gsi_base;
};

struct acpi_iso { /* Interrupt Source Override: ISA IRQ -> GSI remap */
    uint8_t  bus;
    uint8_t  source_irq;
    uint32_t gsi;
    uint16_t flags;
};

struct acpi_info {
    bool     found;
    uint32_t local_apic_addr; /* xAPIC MMIO base, only used as a fallback */

    uint32_t ioapic_count;
    struct acpi_ioapic ioapics[MAX_IOAPICS];

    uint32_t iso_count;
    struct acpi_iso isos[MAX_ISOS];
};

extern struct acpi_info g_acpi;

/* Parses RSDP -> XSDT/RSDT -> MADT. Populates g_acpi. Safe to call even
 * if g_boot.rsdp is NULL (g_acpi.found stays false and IOAPIC bring-up
 * falls back to the architectural default GSI/address layout). */
void acpi_init(void);

/* Maps ISA IRQ `isa_irq` (0-15) to its actual GSI, honouring any
 * Interrupt Source Override found in the MADT. */
uint32_t acpi_isa_irq_to_gsi(uint8_t isa_irq);

#endif /* NEXUS_ACPI_H */
