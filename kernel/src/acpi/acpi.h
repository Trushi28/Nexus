#ifndef NEXUS_ACPI_H
#define NEXUS_ACPI_H

#include "klib/klib.h"

#define MAX_IOAPICS 8
#define MAX_ISOS 32

struct acpi_ioapic {
  uint8_t id;
  uint32_t phys_addr;
  uint32_t gsi_base;
};

struct acpi_iso { /* Interrupt Source Override: ISA IRQ -> GSI remap */
  uint8_t bus;
  uint8_t source_irq;
  uint32_t gsi;
  uint16_t flags;
};

struct acpi_info {
  bool found;
  uint32_t local_apic_addr; /* xAPIC MMIO base, only used as a fallback */

  uint32_t ioapic_count;
  struct acpi_ioapic ioapics[MAX_IOAPICS];

  uint32_t iso_count;
  struct acpi_iso isos[MAX_ISOS];

  /* ------------------------------ shutdown -----------------------------
   * Populated by acpi_init()'s FADT/DSDT walk (see init_shutdown_info()
   * in acpi.c) -- everything acpi_shutdown() needs to power the
   * machine off via the real ACPI \_S5 sleep state. shutdown_ready is
   * the only field callers outside this file should read directly;
   * the rest are acpi_shutdown()'s own working state, exposed here
   * only because C has no "friend function". */
  bool shutdown_ready;
  uint32_t smi_cmd_port;
  uint8_t acpi_enable_value;
  uint32_t pm1a_cnt_port;
  uint32_t pm1b_cnt_port; /* 0 if this machine has no PM1b block */
  uint16_t slp_typa;
  uint16_t slp_typb; /* only meaningful if pm1b_cnt_port != 0 */
};

extern struct acpi_info g_acpi;

/* Parses RSDP -> XSDT/RSDT -> MADT, and separately -> FADT -> DSDT -> the
 * \_S5 sleep package (see acpi_shutdown()). Populates g_acpi. Safe to
 * call even if g_boot.rsdp is NULL, or if any one of these tables is
 * missing -- each missing piece degrades exactly one feature
 * (IOAPIC routing falls back to defaults; shutdown falls back to its
 * own chain) rather than taking boot down. */
void acpi_init(void);

/* Maps ISA IRQ `isa_irq` (0-15) to its actual GSI, honouring any
 * Interrupt Source Override found in the MADT. */
uint32_t acpi_isa_irq_to_gsi(uint8_t isa_irq);

/* True if acpi_init() found a real \_S5 package -- i.e. whether
 * acpi_shutdown() below will do a genuine ACPI S5 shutdown or fall
 * straight to its QEMU-only fallback. Purely informational;
 * acpi_shutdown() always makes a best effort regardless of this. */
bool acpi_shutdown_available(void);

/* Powers the machine off. Tries, in order: (1) a real ACPI \_S5
 * shutdown, if acpi_shutdown_available() -- enabling ACPI mode first
 * if the firmware hasn't already, then writing SLP_TYPa|SLP_EN to
 * PM1a_CNT (and PM1b_CNT, if present); (2) a well-known QEMU/Bochs
 * convenience write to port 0x604, honoured by several ICH9/PIIX4
 * ACPI implementations as a power-off request on its own -- a no-op
 * on real hardware; (3) if neither actually powered the machine off,
 * a plain hlt loop. Never returns. */
NORETURN void acpi_shutdown(void);

#endif /* NEXUS_ACPI_H */
