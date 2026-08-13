#include "apic/lapic.h"
#include "boot/requests.h"
#include "cpu/io.h"
#include "cpu/vectors.h"
#include "mm/vmm.h"

#define IA32_APIC_BASE_MSR 0x1B
#define APIC_BASE_EXTD (1ULL << 10)
#define APIC_BASE_EN (1ULL << 11)

#define REG_ID 0x20
#define REG_EOI 0xB0
#define REG_SVR 0xF0
#define REG_ICR_LOW 0x300
#define REG_ICR_HIGH 0x310
#define REG_LVT_TIMER 0x320
#define REG_LVT_LINT0 0x350
#define REG_LVT_LINT1 0x360
#define REG_LVT_ERROR 0x370
#define REG_TIMER_INIT 0x380
#define REG_TIMER_CUR 0x390
#define REG_TIMER_DIV 0x3E0
#define REG_TPR 0x80

#define ICR_DELIVERY_FIXED (0u << 8)
#define ICR_DELIVERY_INIT (5u << 8)
#define ICR_DELIVERY_STARTUP (6u << 8)
#define ICR_DEST_PHYSICAL (0u << 11)
#define ICR_LEVEL_ASSERT (1u << 14)
#define ICR_TRIGGER_EDGE (0u << 15)
#define ICR_SHORTHAND_NONE (0u << 18)
#define ICR_SHORTHAND_ALL_EXCL_SELF (3u << 18)

#define LVT_MASKED (1u << 16)
#define LVT_TIMER_PERIODIC (1u << 17)

static bool x2apic_mode = false;
static uint8_t *xapic_mmio = NULL; /* only valid if !x2apic_mode */
static bool lapic_ready = false;

static uint32_t xapic_read(uint32_t reg) {
  return *(volatile uint32_t *)(xapic_mmio + reg);
}

static void xapic_write(uint32_t reg, uint32_t val) {
  *(volatile uint32_t *)(xapic_mmio + reg) = val;
}

static uint32_t reg_read(uint32_t xapic_offset) {
  if (x2apic_mode) {
    return (uint32_t)rdmsr(0x800 + (xapic_offset / 0x10));
  }
  return xapic_read(xapic_offset);
}

static void reg_write(uint32_t xapic_offset, uint32_t val) {
  if (x2apic_mode) {
    wrmsr(0x800 + (xapic_offset / 0x10), val);
  } else {
    xapic_write(xapic_offset, val);
  }
}

bool lapic_using_x2apic(void) { return x2apic_mode; }

uint32_t lapic_id(void) {
  if (x2apic_mode) {
    return (uint32_t)rdmsr(0x800 + (REG_ID / 0x10));
  }
  return xapic_read(REG_ID) >> 24;
}

void lapic_init(void) {
  uint32_t eax, ebx, ecx, edx;
  cpuid(1, 0, &eax, &ebx, &ecx, &edx);
  bool cpu_supports_x2apic = (ecx & (1u << 21)) != 0;

  uint64_t base_msr = rdmsr(IA32_APIC_BASE_MSR);

  if (cpu_supports_x2apic) {
    base_msr |= APIC_BASE_EN | APIC_BASE_EXTD;
    wrmsr(IA32_APIC_BASE_MSR, base_msr);
    x2apic_mode = true;
  } else {
    base_msr |= APIC_BASE_EN;
    wrmsr(IA32_APIC_BASE_MSR, base_msr);
    x2apic_mode = false;

    if (xapic_mmio == NULL) {
      uint64_t phys_base = base_msr & 0xFFFFF000ULL;
      xapic_mmio = (uint8_t *)vmm_map_mmio(phys_base, PAGE_SIZE);
    }
  }

  /* Mask the two legacy LINT lines -- we route everything through the
   * I/O APIC instead, so LAPIC-local ExtINT/NMI delivery has no role
   * here and should not be left armed. */
  reg_write(REG_LVT_LINT0, LVT_MASKED);
  reg_write(REG_LVT_LINT1, LVT_MASKED);
  reg_write(REG_LVT_ERROR, VEC_LAPIC_ERROR);

  reg_write(REG_TPR, 0); /* accept every interrupt priority */

  /* Software-enable the APIC and set the spurious-interrupt vector.
   * VEC_SPURIOUS is 0xFF, matching the low-nibble-all-ones convention
   * the APIC spec recommends for the spurious vector. */
  reg_write(REG_SVR, (1u << 8) | VEC_SPURIOUS);
  lapic_ready = true;
}

bool lapic_is_ready(void) { return lapic_ready; }

void lapic_send_eoi(void) { reg_write(REG_EOI, 0); }

static void write_icr(uint32_t dest_apic_id, uint32_t low) {
  if (x2apic_mode) {
    wrmsr(0x830, ((uint64_t)dest_apic_id << 32) | low);
  } else {
    xapic_write(REG_ICR_HIGH, dest_apic_id << 24);
    xapic_write(REG_ICR_LOW, low);
    /* Wait for the previous IPI to actually be sent (xAPIC only --
     * x2APIC IPI sends are not asynchronous in the same way). */
    while (xapic_read(REG_ICR_LOW) & (1u << 12)) {
      asm volatile("pause");
    }
  }
}

void lapic_send_ipi(uint32_t dest_apic_id, uint8_t vector) {
  write_icr(dest_apic_id, ICR_DELIVERY_FIXED | ICR_DEST_PHYSICAL |
                              ICR_LEVEL_ASSERT | ICR_TRIGGER_EDGE | vector);
}

void lapic_send_ipi_all_excluding_self(uint8_t vector) {
  write_icr(0, ICR_DELIVERY_FIXED | ICR_LEVEL_ASSERT | ICR_TRIGGER_EDGE |
                   ICR_SHORTHAND_ALL_EXCL_SELF | vector);
}

void lapic_send_init_ipi(uint32_t dest_apic_id) {
  write_icr(dest_apic_id, ICR_DELIVERY_INIT | ICR_DEST_PHYSICAL |
                              ICR_LEVEL_ASSERT | ICR_TRIGGER_EDGE);
}

void lapic_send_startup_ipi(uint32_t dest_apic_id, uint8_t start_page) {
  write_icr(dest_apic_id, ICR_DELIVERY_STARTUP | ICR_DEST_PHYSICAL |
                              ICR_LEVEL_ASSERT | ICR_TRIGGER_EDGE | start_page);
}

void lapic_timer_stop(void) {
  reg_write(REG_LVT_TIMER, LVT_MASKED);
  reg_write(REG_TIMER_INIT, 0);
}

void lapic_timer_set_divide(uint8_t divide_encoding) {
  reg_write(REG_TIMER_DIV, divide_encoding);
}

void lapic_timer_start_oneshot(uint8_t vector, uint32_t initial_count) {
  reg_write(REG_LVT_TIMER, vector);
  reg_write(REG_TIMER_INIT, initial_count);
}

void lapic_timer_start_oneshot_masked(uint32_t initial_count) {
  reg_write(REG_LVT_TIMER, LVT_MASKED);
  reg_write(REG_TIMER_INIT, initial_count);
}

void lapic_timer_start_periodic(uint8_t vector, uint32_t initial_count) {
  reg_write(REG_LVT_TIMER, vector | LVT_TIMER_PERIODIC);
  reg_write(REG_TIMER_INIT, initial_count);
}

uint32_t lapic_timer_current_count(void) { return reg_read(REG_TIMER_CUR); }
