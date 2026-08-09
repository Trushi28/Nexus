#ifndef NEXUS_PCI_H
#define NEXUS_PCI_H

#include "klib/klib.h"

#define PCI_MAX_DEVICES 64

struct pci_device {
    uint8_t  bus, slot, func;
    uint16_t vendor_id, device_id;
    uint8_t  class_code, subclass, prog_if, revision;
};

/* Brute-force legacy config-space (0xCF8/0xCFC) scan of every bus/
 * device/function. Simple, a little slow (a few tens of milliseconds),
 * plenty fast enough for an interactive "lspci". */
void pci_scan(void);

uint32_t pci_device_count(void);
const struct pci_device *pci_device_at(uint32_t index);

const char *pci_class_name(uint8_t class_code);

#endif /* NEXUS_PCI_H */
