#include "acpi/acpi.h"
#include "boot/requests.h"
#include "cpu/io.h"
#include "debug/log.h"
#include "time/timer.h"

struct acpi_info g_acpi;

struct PACKED rsdp_v1 {
  char sig[8];
  uint8_t checksum;
  char oem_id[6];
  uint8_t revision;
  uint32_t rsdt_address;
};

struct PACKED rsdp_v2 {
  struct rsdp_v1 v1;
  uint32_t length;
  uint64_t xsdt_address;
  uint8_t ext_checksum;
  uint8_t reserved[3];
};

struct PACKED sdt_header {
  char sig[4];
  uint32_t length;
  uint8_t revision;
  uint8_t checksum;
  char oem_id[6];
  char oem_table_id[8];
  uint32_t oem_revision;
  uint32_t creator_id;
  uint32_t creator_revision;
};

struct PACKED madt_entry_header {
  uint8_t type;
  uint8_t length;
};

struct PACKED madt_ioapic {
  struct madt_entry_header hdr;
  uint8_t ioapic_id;
  uint8_t reserved;
  uint32_t ioapic_addr;
  uint32_t gsi_base;
};

struct PACKED madt_iso {
  struct madt_entry_header hdr;
  uint8_t bus;
  uint8_t source;
  uint32_t gsi;
  uint16_t flags;
};

struct PACKED madt_local_apic_addr_override {
  struct madt_entry_header hdr;
  uint16_t reserved;
  uint64_t addr;
};

struct PACKED madt {
  struct sdt_header hdr;
  uint32_t local_apic_addr;
  uint32_t flags;
  uint8_t entries[];
};

#define MADT_TYPE_IOAPIC 1
#define MADT_TYPE_ISO 2
#define MADT_TYPE_LAPIC_ADDR_OVERRIDE 5

struct PACKED fadt {
  struct sdt_header hdr;
  uint32_t firmware_ctrl;
  uint32_t dsdt;
  uint8_t reserved1;
  uint8_t preferred_pm_profile;
  uint16_t sci_int;
  uint32_t smi_cmd;
  uint8_t acpi_enable;
  uint8_t acpi_disable;
  uint8_t s4bios_req;
  uint8_t pstate_cnt;
  uint32_t pm1a_evt_blk;
  uint32_t pm1b_evt_blk;
  uint32_t pm1a_cnt_blk;
  uint32_t pm1b_cnt_blk;
  uint32_t pm2_cnt_blk;
  uint32_t pm_tmr_blk;
  uint32_t gpe0_blk;
  uint32_t gpe1_blk;
  uint8_t pm1_evt_len;
  uint8_t pm1_cnt_len;
  uint8_t pm2_cnt_len;
  uint8_t pm_tmr_len;
  uint8_t gpe0_blk_len;
  uint8_t gpe1_blk_len;
  uint8_t gpe1_base;
  uint8_t cst_cnt;
  uint16_t p_lvl2_lat;
  uint16_t p_lvl3_lat;
  uint16_t flush_size;
  uint16_t flush_stride;
  uint8_t duty_offset;
  uint8_t duty_width;
  uint8_t day_alrm;
  uint8_t mon_alrm;
  uint8_t century;
  uint16_t iapc_boot_arch;
  uint8_t reserved2;
  uint32_t flags;
};
_Static_assert(offsetof(struct fadt, smi_cmd) == 48,
               "FADT layout must match the ACPI spec's byte offsets");
_Static_assert(offsetof(struct fadt, pm1a_cnt_blk) == 64,
               "FADT layout must match the ACPI spec's byte offsets");
_Static_assert(offsetof(struct fadt, pm1_cnt_len) == 89,
               "FADT layout must match the ACPI spec's byte offsets");

/* PM1_CNT's SLP_EN bit (bit 13) -- writing SLP_TYP with this bit set
 * is the actual "go to sleep now" trigger. Same bit position doubles
 * as the value QEMU's port-0x604 convenience hack expects on its own. */
#define PM1_SLP_EN (1u << 13)

static bool checksum_ok(const void *data, size_t len) {
  uint8_t sum = 0;
  const uint8_t *p = (const uint8_t *)data;
  for (size_t i = 0; i < len; i++) {
    sum = (uint8_t)(sum + p[i]);
  }
  return sum == 0;
}

static void parse_madt(struct madt *m) {
  g_acpi.local_apic_addr = m->local_apic_addr;

  uint8_t *p = m->entries;
  uint8_t *end = (uint8_t *)m + m->hdr.length;

  while (p < end) {
    struct madt_entry_header *eh = (struct madt_entry_header *)p;
    if (eh->length == 0) {
      break; /* malformed table -- bail rather than loop forever */
    }

    switch (eh->type) {
    case MADT_TYPE_IOAPIC: {
      struct madt_ioapic *io = (struct madt_ioapic *)p;
      if (g_acpi.ioapic_count < MAX_IOAPICS) {
        struct acpi_ioapic *dst = &g_acpi.ioapics[g_acpi.ioapic_count++];
        dst->id = io->ioapic_id;
        dst->phys_addr = io->ioapic_addr;
        dst->gsi_base = io->gsi_base;
      }
      break;
    }
    case MADT_TYPE_ISO: {
      struct madt_iso *iso = (struct madt_iso *)p;
      if (g_acpi.iso_count < MAX_ISOS) {
        struct acpi_iso *dst = &g_acpi.isos[g_acpi.iso_count++];
        dst->bus = iso->bus;
        dst->source_irq = iso->source;
        dst->gsi = iso->gsi;
        dst->flags = iso->flags;
      }
      break;
    }
    case MADT_TYPE_LAPIC_ADDR_OVERRIDE: {
      struct madt_local_apic_addr_override *ov =
          (struct madt_local_apic_addr_override *)p;
      g_acpi.local_apic_addr = (uint32_t)ov->addr;
      break;
    }
    default:
      break; /* local APIC / x2APIC / NMI entries: not needed here */
    }

    p += eh->length;
  }
}

static struct sdt_header *find_table(void *root_table_ptr, bool is_xsdt,
                                     const char *sig) {
  struct sdt_header *root = (struct sdt_header *)root_table_ptr;
  uint32_t entry_count =
      (root->length - sizeof(struct sdt_header)) / (is_xsdt ? 8 : 4);
  uint8_t *entries = (uint8_t *)root_table_ptr + sizeof(struct sdt_header);

  for (uint32_t i = 0; i < entry_count; i++) {
    uint64_t phys;
    if (is_xsdt) {
      uint64_t val;
      memcpy(&val, entries + i * 8, 8);
      phys = val;
    } else {
      uint32_t val;
      memcpy(&val, entries + i * 4, 4);
      phys = val;
    }
    struct sdt_header *sub = (struct sdt_header *)phys_to_virt(phys);
    if (memcmp(sub->sig, sig, 4) == 0) {
      return sub;
    }
  }
  return NULL;
}

/* Scans `dsdt`'s raw AML bytecode for a top-level "_S5_" NameOp
 * package -- the \_S5 object whose two byte values are what the OS is
 * actually supposed to write into PM1_CNT to power the machine off.
 * There's no real AML interpreter here (see docs/Design.md's
 * philosophy: prove the boring, obviously-correct version first) --
 * this is the well-known "grep the bytecode for _S5_" shortcut every
 * hobby OS's shutdown code eventually reaches for, because a from-
 * scratch AML parser capable of correctly walking the FULL DSDT
 * namespace is a project unto itself just to reach one four-byte
 * object. Every offset below is bounds-checked against the table's
 * own declared length -- a truncated or hostile DSDT fails this scan
 * cleanly (shutdown_ready stays false) rather than reading past it.
 *
 * AML shape being matched, right after the 4-byte "_S5_" NameSeg:
 *   PackageOp(0x12) PkgLength NumElements SLP_TYPa SLP_TYPb ...
 * PkgLength's own first byte's top 2 bits say how many EXTRA length
 * bytes follow (0-3); we don't need the length value itself, only how
 * many bytes to skip to reach NumElements and then the elements.
 * Each element is either a raw small integer (the byte itself, for
 * values 0/1 encoded as ZeroOp/OneOp) or BytePrefix(0x0A) + 1 byte. */
static bool find_s5_package(struct sdt_header *dsdt, uint16_t *slp_typa_out,
                            uint16_t *slp_typb_out) {
  const uint8_t *base = (const uint8_t *)dsdt;
  uint32_t len = dsdt->length;
  if (len < sizeof(struct sdt_header)) {
    return false;
  }

  for (uint32_t i = sizeof(struct sdt_header); i + 4 <= len; i++) {
    if (!(base[i] == '_' && base[i + 1] == 'S' && base[i + 2] == '5' &&
          base[i + 3] == '_')) {
      continue;
    }

    uint32_t p = i + 4;
    if (p >= len || base[p] != 0x12 /* PackageOp */) {
      continue; /* some other "_S5_"-named object -- keep scanning */
    }
    p++;
    if (p >= len) {
      return false;
    }

    uint32_t extra = (base[p] >> 6) & 0x3;
    p += extra + 2; /* PkgLength (1+extra bytes) + NumElements (1 byte) */
    if (p >= len) {
      return false;
    }

    uint16_t slp_typa, slp_typb;

    if (base[p] == 0x0A) { /* BytePrefix -- next byte is the value */
      p++;
      if (p >= len) {
        return false;
      }
    }
    slp_typa =
        (uint16_t)(base[p] << 10); /* SLP_TYP lives at PM1_CNT bits 12:10 */
    p++;
    if (p >= len) {
      return false;
    }

    if (base[p] == 0x0A) {
      p++;
      if (p >= len) {
        return false;
      }
    }
    slp_typb = (uint16_t)(base[p] << 10);

    *slp_typa_out = slp_typa;
    *slp_typb_out = slp_typb;
    return true;
  }
  return false;
}

/* Locates the FADT, and through it the DSDT, and tries to pull the
 * \_S5 sleep-type values out of it -- see find_s5_package()'s comment.
 * Populates g_acpi's shutdown_ready/pm1a_cnt_port/... fields; leaves
 * them all at their zeroed defaults (shutdown_ready == false) if
 * anything along the way is missing, which acpi_shutdown() already
 * treats as "no real ACPI shutdown, fall back" rather than an error --
 * a machine with no usable FADT/DSDT shouldn't take the rest of ACPI
 * bring-up down with it. */
static void init_shutdown_info(void *root, bool is_xsdt) {
  struct sdt_header *fadt_hdr = find_table(root, is_xsdt, "FACP");
  if (fadt_hdr == NULL) {
    kprintf("[acpi] no FADT (FACP) table found -- ACPI shutdown unavailable\n");
    return;
  }
  if (fadt_hdr->length < offsetof(struct fadt, flags) + 4) {
    kprintf("[acpi] FADT too short to hold the fields shutdown needs\n");
    return;
  }
  struct fadt *fadt = (struct fadt *)fadt_hdr;

  g_acpi.smi_cmd_port = fadt->smi_cmd;
  g_acpi.acpi_enable_value = fadt->acpi_enable;
  g_acpi.pm1a_cnt_port = fadt->pm1a_cnt_blk;
  g_acpi.pm1b_cnt_port = fadt->pm1b_cnt_blk;

  if (fadt->dsdt == 0 || g_acpi.pm1a_cnt_port == 0) {
    kprintf("[acpi] FADT has no DSDT pointer or no PM1a_CNT_BLK -- "
            "ACPI shutdown unavailable\n");
    return;
  }

  struct sdt_header *dsdt = (struct sdt_header *)phys_to_virt(fadt->dsdt);
  if (memcmp(dsdt->sig, "DSDT", 4) != 0) {
    kprintf("[acpi] FADT's DSDT pointer doesn't point at a DSDT -- "
            "ACPI shutdown unavailable\n");
    return;
  }

  if (!find_s5_package(dsdt, &g_acpi.slp_typa, &g_acpi.slp_typb)) {
    kprintf("[acpi] no \\_S5 package found in the DSDT -- ACPI shutdown "
            "unavailable ('shutdown' will fall back to the QEMU-only "
            "power-off hack -- see acpi_shutdown())\n");
    return;
  }

  g_acpi.shutdown_ready = true;
  kprintf("[acpi] \\_S5 found: SLP_TYPa=0x%x SLP_TYPb=0x%x, "
          "PM1a_CNT_BLK=0x%x PM1b_CNT_BLK=0x%x\n",
          g_acpi.slp_typa, g_acpi.slp_typb, g_acpi.pm1a_cnt_port,
          g_acpi.pm1b_cnt_port);
}

void acpi_init(void) {
  memset(&g_acpi, 0, sizeof(g_acpi));

  if (g_boot.rsdp == NULL) {
    kprintf("[acpi] no RSDP from the bootloader -- ACPI unavailable\n");
    return;
  }

  struct rsdp_v1 *v1 = (struct rsdp_v1 *)g_boot.rsdp;
  if (memcmp(v1->sig, "RSD PTR ", 8) != 0) {
    kprintf("[acpi] RSDP signature invalid\n");
    return;
  }

  void *root = NULL;
  bool is_xsdt = false;

  if (v1->revision >= 2) {
    struct rsdp_v2 *v2 = (struct rsdp_v2 *)g_boot.rsdp;
    if (checksum_ok(v2, sizeof(*v2)) && v2->xsdt_address != 0) {
      root = phys_to_virt(v2->xsdt_address);
      is_xsdt = true;
    }
  }
  if (root == NULL) {
    if (!checksum_ok(v1, sizeof(*v1))) {
      kprintf("[acpi] RSDP checksum invalid\n");
      return;
    }
    root = phys_to_virt(v1->rsdt_address);
    is_xsdt = false;
  }

  /* MADT and FADT are independent lookups -- a machine missing one
   * shouldn't cost you the other (IOAPIC routing vs. shutdown). */
  struct sdt_header *madt_hdr = find_table(root, is_xsdt, "APIC");
  if (madt_hdr == NULL) {
    kprintf("[acpi] no MADT (APIC) table found -- IOAPIC routing will "
            "fall back to architectural defaults\n");
  } else {
    parse_madt((struct madt *)madt_hdr);
    g_acpi.found = true;
    kprintf("[acpi] MADT parsed: %u I/O APIC(s), %u interrupt "
            "override(s), local APIC MMIO base 0x%x\n",
            g_acpi.ioapic_count, g_acpi.iso_count, g_acpi.local_apic_addr);
  }

  init_shutdown_info(root, is_xsdt);
}

uint32_t acpi_isa_irq_to_gsi(uint8_t isa_irq) {
  for (uint32_t i = 0; i < g_acpi.iso_count; i++) {
    if (g_acpi.isos[i].bus == 0 && g_acpi.isos[i].source_irq == isa_irq) {
      return g_acpi.isos[i].gsi;
    }
  }
  return isa_irq; /* identity mapping is the architectural default */
}

static void enable_acpi_mode(void) {
  if (g_acpi.pm1a_cnt_port != 0 &&
      (inw((uint16_t)g_acpi.pm1a_cnt_port) & 1) != 0) {
    return; /* SCI_EN already set -- already in ACPI mode */
  }
  if (g_acpi.smi_cmd_port == 0 || g_acpi.acpi_enable_value == 0) {
    return; /* no legacy SMI hand-off on this machine -- nothing to do */
  }

  outb((uint16_t)g_acpi.smi_cmd_port, g_acpi.acpi_enable_value);

  uint64_t deadline = timer_uptime_ms() + 100;
  while (g_acpi.pm1a_cnt_port != 0 &&
         (inw((uint16_t)g_acpi.pm1a_cnt_port) & 1) == 0) {
    if (timer_uptime_ms() > deadline) {
      break; /* best effort -- fall through and try the SLP_TYP
                 write anyway */
    }
    asm volatile("pause");
  }
}

bool acpi_shutdown_available(void) { return g_acpi.shutdown_ready; }

NORETURN void acpi_shutdown(void) {
  cli(); // nothing else on this cpu should run between here and the machine
         // actually powering off

  if (g_acpi.shutdown_ready) {
    enable_acpi_mode();

    kprintf("[acpi] powering off via ACPI \\_S5 (PM1a_CNT=0x%x, "
            "SLP_TYPa=0x%x)\n",
            g_acpi.pm1a_cnt_port, g_acpi.slp_typa);
    outw((uint16_t)g_acpi.pm1a_cnt_port,
         (uint16_t)(g_acpi.slp_typa | PM1_SLP_EN));
    if (g_acpi.pm1b_cnt_port != 0) {
      outw((uint16_t)g_acpi.pm1b_cnt_port,
           (uint16_t)(g_acpi.slp_typb | PM1_SLP_EN));
    }

    timer_busy_wait_ms(50);
  } else {
    kprintf("[acpi] no \\_S5 package was found at boot -- going "
            "straight to the fallback below\n");
  }

  outw(0x604, PM1_SLP_EN);
  timer_busy_wait_ms(50);

  kprintf("[acpi] shutdown request didn't power the machine off -- "
          "halting the CPU instead (safe to power off by hand now)\n");
  hang();
}
