#ifndef NEXUS_PCI_H
#define NEXUS_PCI_H

#include "klib/klib.h"

#define PCI_MAX_DEVICES 64
#define PCI_MAX_BARS 6

struct pci_bar {
  bool present;
  bool is_mmio;      /* false = I/O port BAR */
  bool is_64bit;     /* MMIO only: this is the low half of a 64-bit
                         pair; the paired high-dword BAR slot is
                         left !present */
  bool prefetchable; /* MMIO only */
  uint64_t base;     /* physical address (MMIO) or I/O port number */
  uint64_t size;     /* decoded region size, in bytes (MMIO) or ports (I/O) */
};

struct pci_device {
  uint8_t bus, slot, func;
  uint16_t vendor_id, device_id;
  uint8_t class_code, subclass, prog_if, revision;
  struct pci_bar bars[PCI_MAX_BARS];
};

struct pci_msix {
  uint8_t cap_offset;   /* offset of the MSI-X capability in config space */
  uint32_t num_vectors; /* size of the vector table */
  void *table;          /* MMIO-mapped vector table -- opaque here,
                            see the definition in pci.c */
};

/* Brute-force legacy config-space (0xCF8/0xCFC) scan of every bus/
 * device/function. Also decodes and records every found device's BARs
 * (see struct pci_bar) -- a standard, side-effect-free procedure (write
 * all-1s, read back the implemented-bits mask, restore the original
 * value), so doing it for every device up front costs nothing a driver
 * binding to one of them later wouldn't have paid anyway. */
void pci_scan(void);

uint32_t pci_device_count(void);
const struct pci_device *pci_device_at(uint32_t index);

/* Finds the first scanned device matching the given class triple (see
 * the PCI class code spec -- e.g. 0x01/0x08/0x02 is "Mass Storage /
 * Non-Volatile Memory / NVMe"). Returns NULL if pci_scan() hasn't run
 * yet or nothing matched. Valid only until the next pci_scan() call. */
const struct pci_device *pci_find_by_class(uint8_t class_code, uint8_t subclass,
                                           uint8_t prog_if);

const char *pci_class_name(uint8_t class_code);

/* ------------------------- raw config-space access -------------------
 * Legacy mechanism #1 (0xCF8 address / 0xCFC data) -- every x86-64
 * chipset still supports it even though MMCONFIG/ECAM exists too, and
 * it's plenty for the handful of devices Nexus talks to directly.
 * Internally locked (a config access is really two I/O ops -- address,
 * then data -- that must not interleave with another CPU's), so safe
 * to call concurrently from anywhere, interrupt context included. */
uint32_t pci_cfg_read32(uint8_t bus, uint8_t slot, uint8_t func,
                        uint8_t offset);
uint16_t pci_cfg_read16(uint8_t bus, uint8_t slot, uint8_t func,
                        uint8_t offset);
uint8_t pci_cfg_read8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void pci_cfg_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset,
                     uint32_t val);
void pci_cfg_write16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset,
                     uint16_t val);
void pci_cfg_write8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset,
                    uint8_t val);

/* Sets Memory Space Enable + Bus Master Enable in the command register.
 * Every driver that actually talks to its device (as opposed to just
 * enumerating it, like lspci) needs this first -- without Bus Master
 * the device is physically incapable of issuing DMA, which is how NVMe
 * (and most modern hardware) does everything. */
void pci_enable_device(const struct pci_device *dev);

#define PCI_CAP_ID_MSI 0x05
#define PCI_CAP_ID_MSIX 0x11

/* Walks the device's capability list (PCI status register bit 4 says
 * whether it has one) looking for `cap_id`. False if there's no list,
 * or none of its entries match. */
bool pci_find_capability(const struct pci_device *dev, uint8_t cap_id,
                         uint8_t *cap_offset_out);

/* Locates, MMIO-maps, and fully masks a device's MSI-X vector table.
 * Requires pci_scan() to have already decoded the BARs (it always has)
 * and the table's declared BAR to actually be implemented. False if the
 * device has no MSI-X capability at all. */
bool pci_msix_init(struct pci_device *dev, struct pci_msix *out);

/* Programs vector table entry `index` to deliver `vector` (a value
 * previously passed to register_interrupt_handler()) to the CPU whose
 * Local APIC ID is `dest_apic_id`, and unmasks that entry. Install the
 * handler BEFORE calling this -- the device is free to fire the moment
 * the entry is unmasked, MSI-X Enable or not. */
void pci_msix_set_vector(struct pci_device *dev, struct pci_msix *msix,
                         uint32_t index, uint32_t dest_apic_id, uint8_t vector);

/* Sets the capability's MSI-X Enable bit (and clears the whole-function
 * mask). Call once, after every vector you intend to use has already
 * been pci_msix_set_vector()'d. */
void pci_msix_enable(struct pci_device *dev, struct pci_msix *msix);

#endif /* NEXUS_PCI_H */
