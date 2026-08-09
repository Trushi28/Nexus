#ifndef NEXUS_REQUESTS_H
#define NEXUS_REQUESTS_H

#include "klib/klib.h"
#include <limine.h>

/*
 * Everything the rest of the kernel needs out of the Limine handoff,
 * pre-validated and exposed as plain globals. boot_requests_init() must
 * run first, before anything else touches these.
 */

struct boot_info {
    bool     base_revision_ok;

    const char *bootloader_name;
    const char *bootloader_version;

    uint64_t firmware_type;      /* LIMINE_FIRMWARE_TYPE_* */

    uint64_t hhdm_offset;

    struct limine_framebuffer *fb; /* NULL if none available */

    uint64_t paging_mode;        /* LIMINE_PAGING_MODE_X86_64_* actually enacted */

    struct limine_mp_response *mp; /* NULL if the MP feature is unsupported */

    struct limine_memmap_response *memmap;

    uint64_t exec_phys_base;
    uint64_t exec_virt_base;

    struct limine_file **modules; /* NULL if none were loaded */
    uint64_t module_count;

    void    *rsdp;                /* HHDM-mapped, ready to use directly */

    bool     tsc_freq_known;
    uint64_t tsc_freq_hz;

    const char *cmdline;          /* never NULL, may be empty string */

    bool     nosmp;                /* "nosmp" present on the cmdline */
};

extern struct boot_info g_boot;

void boot_requests_init(void);

/* Finds a Limine module whose configured path ends with `path_suffix`
 * (e.g. "initrd.tar" matches a limine.conf `module_path:` of
 * "boot():/boot/initrd.tar" regardless of which boot device it came
 * from). Returns NULL if none matched. */
const struct limine_file *boot_find_module(const char *path_suffix);

static inline void *phys_to_virt(uint64_t phys) {
    return (void *)(phys + g_boot.hhdm_offset);
}

static inline uint64_t virt_to_phys_hhdm(const void *virt) {
    return (uint64_t)virt - g_boot.hhdm_offset;
}

#endif /* NEXUS_REQUESTS_H */
