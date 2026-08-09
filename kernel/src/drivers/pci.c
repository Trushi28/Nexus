#include "drivers/pci.h"
#include "cpu/io.h"

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

static struct pci_device devices[PCI_MAX_DEVICES];
static uint32_t device_count = 0;

static uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t address = (1u << 31) | ((uint32_t)bus << 16) | ((uint32_t)slot << 11) |
                        ((uint32_t)func << 8) | (offset & 0xFC);
    outl(PCI_CONFIG_ADDRESS, address);
    return inl(PCI_CONFIG_DATA);
}

void pci_scan(void) {
    device_count = 0;

    for (uint32_t bus = 0; bus < 256; bus++) {
        for (uint32_t slot = 0; slot < 32; slot++) {
            for (uint32_t func = 0; func < 8; func++) {
                uint32_t id = pci_read32((uint8_t)bus, (uint8_t)slot, (uint8_t)func, 0x00);
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

                    uint32_t classreg = pci_read32((uint8_t)bus, (uint8_t)slot, (uint8_t)func, 0x08);
                    d->revision   = classreg & 0xFF;
                    d->prog_if    = (classreg >> 8) & 0xFF;
                    d->subclass   = (classreg >> 16) & 0xFF;
                    d->class_code = (classreg >> 24) & 0xFF;
                }

                if (func == 0) {
                    uint32_t header = pci_read32((uint8_t)bus, (uint8_t)slot, 0, 0x0C);
                    bool multifunction = ((header >> 16) & 0x80) != 0;
                    if (!multifunction) {
                        break; /* only function 0 exists */
                    }
                }
            }
        }
    }
}

uint32_t pci_device_count(void) {
    return device_count;
}

const struct pci_device *pci_device_at(uint32_t index) {
    if (index >= device_count) {
        return NULL;
    }
    return &devices[index];
}

const char *pci_class_name(uint8_t class_code) {
    switch (class_code) {
    case 0x00: return "Unclassified";
    case 0x01: return "Mass Storage Controller";
    case 0x02: return "Network Controller";
    case 0x03: return "Display Controller";
    case 0x04: return "Multimedia Controller";
    case 0x05: return "Memory Controller";
    case 0x06: return "Bridge";
    case 0x07: return "Communication Controller";
    case 0x08: return "Base System Peripheral";
    case 0x09: return "Input Device Controller";
    case 0x0A: return "Docking Station";
    case 0x0B: return "Processor";
    case 0x0C: return "Serial Bus Controller";
    case 0x0D: return "Wireless Controller";
    case 0x0E: return "Intelligent Controller";
    case 0x0F: return "Satellite Communication Controller";
    case 0x10: return "Encryption Controller";
    case 0x11: return "Signal Processing Controller";
    default:   return "Unknown";
    }
}
