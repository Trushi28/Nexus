#ifndef NEXUS_PCI_H
#define NEXUS_PCI_H

#include "klib/klib.h"

#define PCI_MAX_DEVICES 64
#define PCI_MAX_BARS 6

struct pci_bar {
  bool present;
  bool is_mmio;      // false = I/O port BAR
  bool is_64bit;     // MMIO only: this is the low half of a 64-bit pair
  bool prefetchable; // MMIO only
  uint64_t base;     // physical address (MMIO) or I/O port number
  uint64_t size;     // decoded region size, in bytes (MMIO) or ports (I/O)
};

struct pci_device {
  uint8_t bus, slot, func;
  uint16_t vendor_id, device_id;
  uint8_t class_code, subclass, prog_if, revision;
  struct pci_bar bars[PCI_MAX_BARS];
};

struct pci_msix {
  uint8_t cap_offset;
  uint32_t num_vectors;
  void *table; // MMIO-mapped vector table
};

// Brute-force config-space scan of every bus/device/function, also decoding BARs.
void pci_scan(void);

uint32_t pci_device_count(void);
const struct pci_device *pci_device_at(uint32_t index);

/* First device matching the given class triple, e.g. 0x01/0x08/0x02 is
 * NVMe. NULL if pci_scan() hasn't run or nothing matched. */
const struct pci_device *pci_find_by_class(uint8_t class_code, uint8_t subclass,
                                           uint8_t prog_if);

const char *pci_class_name(uint8_t class_code);

// Legacy 0xCF8/0xCFC config-space access -- internally locked, safe from any context.
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

// Sets Memory Space Enable + Bus Master Enable -- required before any DMA.
void pci_enable_device(const struct pci_device *dev);

#define PCI_CAP_ID_MSI 0x05
#define PCI_CAP_ID_MSIX 0x11

bool pci_find_capability(const struct pci_device *dev, uint8_t cap_id,
                         uint8_t *cap_offset_out);

/* Locates, maps, and fully masks a device's MSI-X vector table. False
 * if the device has no MSI-X capability. */
bool pci_msix_init(struct pci_device *dev, struct pci_msix *out);

/* Programs and unmasks vector table entry `index`. Install the handler
 * BEFORE calling this -- the device may fire the moment it's unmasked. */
void pci_msix_set_vector(struct pci_device *dev, struct pci_msix *msix,
                         uint32_t index, uint32_t dest_apic_id, uint8_t vector);

// Call once, after every vector is already pci_msix_set_vector()'d.
void pci_msix_enable(struct pci_device *dev, struct pci_msix *msix);

#endif /* NEXUS_PCI_H */
