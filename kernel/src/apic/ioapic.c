#include "acpi/acpi.h"
#include "apic/ioapic.h"
#include "cpu/io.h"
#include "debug/log.h"
#include "mm/vmm.h"
#include "panic.h"

#define IOAPICID 0x00
#define IOAPICVER 0x01
#define IOREDTBL 0x10

struct ioapic_dev {
  volatile uint8_t *mmio;
  uint32_t gsi_base;
  uint32_t gsi_count;
};

#define MAX_IOAPIC_DEVS 8
static struct ioapic_dev devs[MAX_IOAPIC_DEVS];
static uint32_t dev_count = 0;

static uint32_t ioapic_read(struct ioapic_dev *d, uint32_t reg) {
  *(volatile uint32_t *)(d->mmio + 0x00) = reg;
  return *(volatile uint32_t *)(d->mmio + 0x10);
}

static void ioapic_write(struct ioapic_dev *d, uint32_t reg, uint32_t val) {
  *(volatile uint32_t *)(d->mmio + 0x00) = reg;
  *(volatile uint32_t *)(d->mmio + 0x10) = val;
}

static void add_ioapic(uint32_t phys_addr, uint32_t gsi_base) {
  if (dev_count >= MAX_IOAPIC_DEVS) {
    return;
  }
  struct ioapic_dev *d = &devs[dev_count++];
  d->mmio = (volatile uint8_t *)vmm_map_mmio(phys_addr, PAGE_SIZE);
  d->gsi_base = gsi_base;
  d->gsi_count = ((ioapic_read(d, IOAPICVER) >> 16) & 0xFF) + 1;
}

void ioapic_init(void) {
  /* Unconditionally mask the legacy 8259 PIC. UEFI does not guarantee
   * it is disabled, which allows phantom hardware interrupts to escape. */
  outb(0x21, 0xFF);
  outb(0xA1, 0xFF);

  if (g_acpi.found && g_acpi.ioapic_count > 0) {
    for (uint32_t i = 0; i < g_acpi.ioapic_count; i++) {
      add_ioapic(g_acpi.ioapics[i].phys_addr, g_acpi.ioapics[i].gsi_base);
    }
  } else {
    kprintf("[ioapic] no MADT I/O APIC entries -- assuming the "
            "architectural default at 0xFEC00000\n");
    add_ioapic(0xFEC00000, 0);
  }

  for (uint32_t i = 0; i < dev_count; i++) {
    kprintf("[ioapic] #%u at 0x%p, GSIs %u-%u\n", i, (void *)devs[i].mmio,
            devs[i].gsi_base, devs[i].gsi_base + devs[i].gsi_count - 1);
  }
}

static struct ioapic_dev *dev_for_gsi(uint32_t gsi) {
  for (uint32_t i = 0; i < dev_count; i++) {
    if (gsi >= devs[i].gsi_base && gsi < devs[i].gsi_base + devs[i].gsi_count) {
      return &devs[i];
    }
  }
  return NULL;
}

void ioapic_set_irq(uint8_t isa_irq, uint8_t vector, uint32_t dest_apic_id) {
  uint32_t gsi = acpi_isa_irq_to_gsi(isa_irq);
  struct ioapic_dev *d = dev_for_gsi(gsi);

  if (d == NULL) {
    panic("ioapic: no I/O APIC owns GSI %u (ISA IRQ %u)", gsi, isa_irq);
  }

  uint16_t iso_flags = 0;
  for (uint32_t i = 0; i < g_acpi.iso_count; i++) {
    if (g_acpi.isos[i].bus == 0 && g_acpi.isos[i].source_irq == isa_irq) {
      iso_flags = g_acpi.isos[i].flags;
      break;
    }
  }

  uint32_t low = vector;
  uint32_t polarity = iso_flags & 0x3;
  uint32_t trigger = (iso_flags >> 2) & 0x3;

  if (polarity == 0x3) {
    low |= (1u << 13); /* active low */
  }
  if (trigger == 0x3) {
    low |= (1u << 15); /* level triggered */
  }

  uint32_t entry_index = gsi - d->gsi_base;
  uint32_t reg = IOREDTBL + entry_index * 2;

  /* CRITICAL: Always write the high 32 bits (Destination APIC) BEFORE writing
   * the low 32 bits (Unmask). Writing low first instantly enables the
   * interrupt, causing a race condition that fires to APIC ID 0x0. */
  ioapic_write(d, reg + 1, dest_apic_id << 24);
  ioapic_write(d, reg, low);
}

void ioapic_mask_irq(uint8_t isa_irq) {
  uint32_t gsi = acpi_isa_irq_to_gsi(isa_irq);
  struct ioapic_dev *d = dev_for_gsi(gsi);
  if (d == NULL) {
    return;
  }

  uint32_t entry_index = gsi - d->gsi_base;
  uint32_t reg = IOREDTBL + entry_index * 2;
  uint32_t low = ioapic_read(d, reg);
  ioapic_write(d, reg, low | (1u << 16));
}
