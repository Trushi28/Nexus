#include "acpi/acpi.h"
#include "boot/requests.h"
#include "debug/log.h"

struct acpi_info g_acpi;

struct PACKED rsdp_v1 {
    char     sig[8];
    uint8_t  checksum;
    char     oem_id[6];
    uint8_t  revision;
    uint32_t rsdt_address;
};

struct PACKED rsdp_v2 {
    struct rsdp_v1 v1;
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t  ext_checksum;
    uint8_t  reserved[3];
};

struct PACKED sdt_header {
    char     sig[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
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
    uint8_t  ioapic_id;
    uint8_t  reserved;
    uint32_t ioapic_addr;
    uint32_t gsi_base;
};

struct PACKED madt_iso {
    struct madt_entry_header hdr;
    uint8_t  bus;
    uint8_t  source;
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
    uint8_t  entries[];
};

#define MADT_TYPE_IOAPIC 1
#define MADT_TYPE_ISO    2
#define MADT_TYPE_LAPIC_ADDR_OVERRIDE 5

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

static struct sdt_header *find_table(void *root_table_ptr, bool is_xsdt, const char *sig) {
    struct sdt_header *root = (struct sdt_header *)root_table_ptr;
    uint32_t entry_count = (root->length - sizeof(struct sdt_header)) / (is_xsdt ? 8 : 4);
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
    bool  is_xsdt = false;

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

    struct sdt_header *madt_hdr = find_table(root, is_xsdt, "APIC");
    if (madt_hdr == NULL) {
        kprintf("[acpi] no MADT (APIC) table found\n");
        return;
    }

    parse_madt((struct madt *)madt_hdr);
    g_acpi.found = true;

    kprintf("[acpi] MADT parsed: %u I/O APIC(s), %u interrupt override(s), "
            "local APIC MMIO base 0x%x\n",
            g_acpi.ioapic_count, g_acpi.iso_count, g_acpi.local_apic_addr);
}

uint32_t acpi_isa_irq_to_gsi(uint8_t isa_irq) {
    for (uint32_t i = 0; i < g_acpi.iso_count; i++) {
        if (g_acpi.isos[i].bus == 0 && g_acpi.isos[i].source_irq == isa_irq) {
            return g_acpi.isos[i].gsi;
        }
    }
    return isa_irq; /* identity mapping is the architectural default */
}
