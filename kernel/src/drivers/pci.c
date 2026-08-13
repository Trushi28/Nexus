#include "drivers/pci.h"
#include "cpu/io.h"
#include "debug/log.h"
#include "mm/vmm.h"
#include "sync/spinlock.h"

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA 0xCFC

#define PCI_STATUS_CAP_LIST (1u << 4)

#define PCI_CMD_MEM_SPACE (1u << 1)
#define PCI_CMD_BUS_MASTER (1u << 2)

#define MSIX_MSG_CTRL_ENABLE (1u << 15)
#define MSIX_MSG_CTRL_FUNC_MASK (1u << 14)
#define MSIX_VECTOR_MASKED (1u << 0)

static struct pci_device devices[PCI_MAX_DEVICES];
static uint32_t device_count = 0;

/* Guards the address-then-data I/O pair below: a config access is
 * really two separate port ops, and another CPU's access interleaving
 * between them would silently hit the wrong device's register. Nothing
 * exercised this before (PCI access only ever happened from the
 * single-threaded shell's `lspci`), but interrupt-context driver code
 * (NVMe's eventual completion handler, for one) changes that. */
static spinlock_t pci_cfg_lock = SPINLOCK_INIT;

static uint32_t cfg_address(uint8_t bus, uint8_t slot, uint8_t func,
                            uint8_t offset) {
  return (1u << 31) | ((uint32_t)bus << 16) | ((uint32_t)slot << 11) |
         ((uint32_t)func << 8) | (offset & 0xFC);
}

uint32_t pci_cfg_read32(uint8_t bus, uint8_t slot, uint8_t func,
                        uint8_t offset) {
  uint64_t f = spinlock_acquire_irqsave(&pci_cfg_lock);
  outl(PCI_CONFIG_ADDRESS, cfg_address(bus, slot, func, offset));
  uint32_t v = inl(PCI_CONFIG_DATA);
  spinlock_release_irqrestore(&pci_cfg_lock, f);
  return v;
}

uint16_t pci_cfg_read16(uint8_t bus, uint8_t slot, uint8_t func,
                        uint8_t offset) {
  uint64_t f = spinlock_acquire_irqsave(&pci_cfg_lock);
  outl(PCI_CONFIG_ADDRESS, cfg_address(bus, slot, func, offset));
  uint16_t v = inw(PCI_CONFIG_DATA + (offset & 2));
  spinlock_release_irqrestore(&pci_cfg_lock, f);
  return v;
}

uint8_t pci_cfg_read8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
  uint64_t f = spinlock_acquire_irqsave(&pci_cfg_lock);
  outl(PCI_CONFIG_ADDRESS, cfg_address(bus, slot, func, offset));
  uint8_t v = inb(PCI_CONFIG_DATA + (offset & 3));
  spinlock_release_irqrestore(&pci_cfg_lock, f);
  return v;
}

void pci_cfg_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset,
                     uint32_t val) {
  uint64_t f = spinlock_acquire_irqsave(&pci_cfg_lock);
  outl(PCI_CONFIG_ADDRESS, cfg_address(bus, slot, func, offset));
  outl(PCI_CONFIG_DATA, val);
  spinlock_release_irqrestore(&pci_cfg_lock, f);
}

void pci_cfg_write16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset,
                     uint16_t val) {
  uint64_t f = spinlock_acquire_irqsave(&pci_cfg_lock);
  outl(PCI_CONFIG_ADDRESS, cfg_address(bus, slot, func, offset));
  outw(PCI_CONFIG_DATA + (offset & 2), val);
  spinlock_release_irqrestore(&pci_cfg_lock, f);
}

void pci_cfg_write8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset,
                    uint8_t val) {
  uint64_t f = spinlock_acquire_irqsave(&pci_cfg_lock);
  outl(PCI_CONFIG_ADDRESS, cfg_address(bus, slot, func, offset));
  outb(PCI_CONFIG_DATA + (offset & 3), val);
  spinlock_release_irqrestore(&pci_cfg_lock, f);
}

/* --------------------------- BAR decoding ---------------------------- */

/* `readback_after_allones` is what you get reading a BAR register back
 * after writing all-1s to it: bits the device actually implements come
 * back 1, the hardwired-0 bits (which encode the region's size) stay 0.
 * `low_bits_mask` strips the low, non-size flag bits first (0x3 for an
 * I/O BAR, 0xF for a memory BAR). */
static uint64_t decode_bar_size(uint32_t readback_after_allones,
                                uint32_t low_bits_mask) {
  uint32_t size_bits = readback_after_allones & ~low_bits_mask;
  if (size_bits == 0) {
    return 0; /* not implemented, or the unused high half of a 64-bit pair */
  }
  return (uint64_t)(~size_bits + 1);
}

static void probe_bars(struct pci_device *dev) {
  for (int i = 0; i < PCI_MAX_BARS; i++) {
    dev->bars[i].present = false;
  }

  for (int i = 0; i < PCI_MAX_BARS; i++) {
    uint8_t off = (uint8_t)(0x10 + i * 4);
    uint32_t orig = pci_cfg_read32(dev->bus, dev->slot, dev->func, off);
    if (orig == 0) {
      continue;
    }

    struct pci_bar *bar = &dev->bars[i];

    if (orig & 1) {
      /* I/O space BAR. Nothing here needs one yet (NVMe, like
       * most modern hardware, is MMIO-only) -- decode it anyway
       * so lspci can show it and the array stays consistent. */
      pci_cfg_write32(dev->bus, dev->slot, dev->func, off, 0xFFFFFFFF);
      uint32_t readback = pci_cfg_read32(dev->bus, dev->slot, dev->func, off);
      pci_cfg_write32(dev->bus, dev->slot, dev->func, off, orig);

      bar->present = true;
      bar->is_mmio = false;
      bar->base = orig & ~0x3u;
      bar->size = decode_bar_size(readback, 0x3);
      continue;
    }

    uint32_t type = (orig >> 1) & 0x3;
    bool is_64 = (type == 0x2);
    bool prefetchable = (orig & (1u << 3)) != 0;

    pci_cfg_write32(dev->bus, dev->slot, dev->func, off, 0xFFFFFFFF);
    uint32_t low_readback = pci_cfg_read32(dev->bus, dev->slot, dev->func, off);
    pci_cfg_write32(dev->bus, dev->slot, dev->func, off, orig);

    uint64_t base = orig & ~0xFu;
    uint64_t size = decode_bar_size(low_readback, 0xF);

    if (is_64) {
      if (i + 1 >= PCI_MAX_BARS) {
        break; /* malformed device -- a 64-bit BAR in the last slot */
      }
      uint8_t hi_off = (uint8_t)(off + 4);
      uint32_t orig_hi = pci_cfg_read32(dev->bus, dev->slot, dev->func, hi_off);

      pci_cfg_write32(dev->bus, dev->slot, dev->func, hi_off, 0xFFFFFFFF);
      uint32_t hi_readback =
          pci_cfg_read32(dev->bus, dev->slot, dev->func, hi_off);
      pci_cfg_write32(dev->bus, dev->slot, dev->func, hi_off, orig_hi);

      base |= (uint64_t)orig_hi << 32;
      if (hi_readback != 0) {
        /* A region bigger than 4GiB carries into the high dword
         * -- combine both halves before inverting. Essentially
         * never happens for anything this kernel talks to, but
         * cheap to get right. */
        uint64_t combined =
            ((uint64_t)hi_readback << 32) | (low_readback & ~0xFull);
        size = ~combined + 1;
      }

      i++; /* the next slot is this BAR's high dword, already consumed */
    }

    bar->present = true;
    bar->is_mmio = true;
    bar->is_64bit = is_64;
    bar->prefetchable = prefetchable;
    bar->base = base;
    bar->size = size;
  }
}

/* ------------------------------- scan --------------------------------- */

void pci_scan(void) {
  device_count = 0;

  for (uint32_t bus = 0; bus < 256; bus++) {
    for (uint32_t slot = 0; slot < 32; slot++) {
      for (uint32_t func = 0; func < 8; func++) {
        uint32_t id =
            pci_cfg_read32((uint8_t)bus, (uint8_t)slot, (uint8_t)func, 0x00);
        uint16_t vendor = id & 0xFFFF;
        if (vendor == 0xFFFF) {
          if (func == 0) {
            break; /* no device at all in this slot */
          }
          continue;
        }

        if (device_count < PCI_MAX_DEVICES) {
          struct pci_device *d = &devices[device_count++];
          d->bus = (uint8_t)bus;
          d->slot = (uint8_t)slot;
          d->func = (uint8_t)func;
          d->vendor_id = vendor;
          d->device_id = (id >> 16) & 0xFFFF;

          uint32_t classreg =
              pci_cfg_read32((uint8_t)bus, (uint8_t)slot, (uint8_t)func, 0x08);
          d->revision = classreg & 0xFF;
          d->prog_if = (classreg >> 8) & 0xFF;
          d->subclass = (classreg >> 16) & 0xFF;
          d->class_code = (classreg >> 24) & 0xFF;

          probe_bars(d);
        }

        if (func == 0) {
          uint32_t header =
              pci_cfg_read32((uint8_t)bus, (uint8_t)slot, 0, 0x0C);
          bool multifunction = ((header >> 16) & 0x80) != 0;
          if (!multifunction) {
            break; /* only function 0 exists */
          }
        }
      }
    }
  }
}

uint32_t pci_device_count(void) { return device_count; }

const struct pci_device *pci_device_at(uint32_t index) {
  if (index >= device_count) {
    return NULL;
  }
  return &devices[index];
}

const struct pci_device *pci_find_by_class(uint8_t class_code, uint8_t subclass,
                                           uint8_t prog_if) {
  for (uint32_t i = 0; i < device_count; i++) {
    struct pci_device *d = &devices[i];
    if (d->class_code == class_code && d->subclass == subclass &&
        d->prog_if == prog_if) {
      return d;
    }
  }
  return NULL;
}

const char *pci_class_name(uint8_t class_code) {
  switch (class_code) {
  case 0x00:
    return "Unclassified";
  case 0x01:
    return "Mass Storage Controller";
  case 0x02:
    return "Network Controller";
  case 0x03:
    return "Display Controller";
  case 0x04:
    return "Multimedia Controller";
  case 0x05:
    return "Memory Controller";
  case 0x06:
    return "Bridge";
  case 0x07:
    return "Communication Controller";
  case 0x08:
    return "Base System Peripheral";
  case 0x09:
    return "Input Device Controller";
  case 0x0A:
    return "Docking Station";
  case 0x0B:
    return "Processor";
  case 0x0C:
    return "Serial Bus Controller";
  case 0x0D:
    return "Wireless Controller";
  case 0x0E:
    return "Intelligent Controller";
  case 0x0F:
    return "Satellite Communication Controller";
  case 0x10:
    return "Encryption Controller";
  case 0x11:
    return "Signal Processing Controller";
  default:
    return "Unknown";
  }
}

/* ------------------------- enable / capabilities ----------------------- */

void pci_enable_device(const struct pci_device *dev) {
  uint16_t cmd = pci_cfg_read16(dev->bus, dev->slot, dev->func, 0x04);
  cmd |= PCI_CMD_MEM_SPACE | PCI_CMD_BUS_MASTER;
  pci_cfg_write16(dev->bus, dev->slot, dev->func, 0x04, cmd);
}

bool pci_find_capability(const struct pci_device *dev, uint8_t cap_id,
                         uint8_t *cap_offset_out) {
  uint16_t status = pci_cfg_read16(dev->bus, dev->slot, dev->func, 0x06);
  if (!(status & PCI_STATUS_CAP_LIST)) {
    return false;
  }

  uint8_t ptr = pci_cfg_read8(dev->bus, dev->slot, dev->func, 0x34) & 0xFC;
  int guard = 0; /* a malformed cap chain could cycle -- bound the walk
                     rather than trust it to terminate */
  while (ptr != 0 && guard++ < 64) {
    uint8_t id = pci_cfg_read8(dev->bus, dev->slot, dev->func, ptr);
    if (id == cap_id) {
      *cap_offset_out = ptr;
      return true;
    }
    ptr = pci_cfg_read8(dev->bus, dev->slot, dev->func, (uint8_t)(ptr + 1)) &
          0xFC;
  }
  return false;
}

/* --------------------------------- MSI-X -------------------------------- */

struct pci_msix_table_entry {
  uint32_t msg_addr_lo;
  uint32_t msg_addr_hi;
  uint32_t msg_data;
  uint32_t vector_control;
};

bool pci_msix_init(struct pci_device *dev, struct pci_msix *out) {
  uint8_t cap;
  if (!pci_find_capability(dev, PCI_CAP_ID_MSIX, &cap)) {
    return false;
  }

  uint16_t msg_ctrl =
      pci_cfg_read16(dev->bus, dev->slot, dev->func, (uint8_t)(cap + 2));
  uint32_t table_info =
      pci_cfg_read32(dev->bus, dev->slot, dev->func, (uint8_t)(cap + 4));

  uint8_t table_bir = table_info & 0x7;
  uint32_t table_bar_offset = table_info & ~0x7u;

  if (table_bir >= PCI_MAX_BARS || !dev->bars[table_bir].present) {
    kprintf("[pci] %02x:%02x.%x: MSI-X table BIR %u has no matching BAR\n",
            dev->bus, dev->slot, dev->func, table_bir);
    return false;
  }

  out->cap_offset = cap;
  out->num_vectors = (uint32_t)(msg_ctrl & 0x7FF) + 1;

  volatile struct pci_msix_table_entry *table =
      (volatile struct pci_msix_table_entry *)vmm_map_mmio(
          dev->bars[table_bir].base + table_bar_offset,
          out->num_vectors * sizeof(struct pci_msix_table_entry));
  out->table = (void *)table;

  /* Mask every entry until the driver explicitly arms the ones it
   * wants -- an unconfigured entry (address/vector still zero) firing
   * on live hardware before anything can handle it is miserable to
   * debug. */
  for (uint32_t i = 0; i < out->num_vectors; i++) {
    table[i].vector_control = MSIX_VECTOR_MASKED;
  }

  return true;
}

void pci_msix_set_vector(struct pci_device *dev, struct pci_msix *msix,
                         uint32_t index, uint32_t dest_apic_id,
                         uint8_t vector) {
  (void)dev;
  volatile struct pci_msix_table_entry *table =
      (volatile struct pci_msix_table_entry *)msix->table;

  /* Fixed delivery, edge-triggered, physical destination -- the
   * standard x86 MSI address/data encoding. Only encodes an 8-bit
   * destination APIC ID (address bits 12-19); a machine with more
   * than 256 CPUs would need the x2APIC extended-destination-ID
   * scheme -- moot here, MAX_CPUS is 256. */
  table[index].msg_addr_lo = 0xFEE00000u | ((dest_apic_id & 0xFFu) << 12);
  table[index].msg_addr_hi = 0;
  table[index].msg_data = vector;
  table[index].vector_control = 0; /* unmask this one entry */
}

void pci_msix_enable(struct pci_device *dev, struct pci_msix *msix) {
  uint16_t msg_ctrl = pci_cfg_read16(dev->bus, dev->slot, dev->func,
                                     (uint8_t)(msix->cap_offset + 2));
  msg_ctrl |= MSIX_MSG_CTRL_ENABLE;
  msg_ctrl &= ~MSIX_MSG_CTRL_FUNC_MASK;
  pci_cfg_write16(dev->bus, dev->slot, dev->func,
                  (uint8_t)(msix->cap_offset + 2), msg_ctrl);
}
