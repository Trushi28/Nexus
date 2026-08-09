#include "boot/requests.h"
#include "debug/serial.h"
#include "cpu/io.h"

struct boot_info g_boot;

/* ---------------------------------------------------------------------
 * The Limine requests section. Every request below is a well-known,
 * stable feature: bootloader info, firmware type, HHDM offset, a
 * framebuffer, explicit 4-level paging, MP (SMP) bring-up (with x2APIC
 * enabled if available), the memory map, our own load address, the ACPI
 * RSDP, our command line, and (best-effort) the TSC frequency.
 *
 * NOTE on the marker/base-revision macros: LIMINE_REQUESTS_START_MARKER,
 * LIMINE_REQUESTS_END_MARKER and LIMINE_BASE_REVISION(N) all expand to a
 * *bare brace-enclosed initializer list*, not a full declaration -- so
 * each needs an explicit `uint64_t foo[N] = MACRO;` around it, unlike
 * some older tutorials floating around that assume otherwise.
 * --------------------------------------------------------------------- */

__attribute__((used, section(".requests_start_marker")))
static volatile uint64_t requests_start_marker[4] = LIMINE_REQUESTS_START_MARKER;

/* Base revision 6 is, as of this writing, the newest and recommended
 * base revision: among other things it guarantees a well-defined LAPIC
 * and control-register state on entry, which our GDT/IDT/APIC bring-up
 * code relies on rather than guessing. */
__attribute__((used, section(".requests")))
static volatile uint64_t base_revision[3] = LIMINE_BASE_REVISION(6);

__attribute__((used, section(".requests")))
static volatile struct limine_bootloader_info_request bootloader_info_request = {
    .id = LIMINE_BOOTLOADER_INFO_REQUEST_ID,
    .revision = 0,
};

__attribute__((used, section(".requests")))
static volatile struct limine_firmware_type_request firmware_type_request = {
    .id = LIMINE_FIRMWARE_TYPE_REQUEST_ID,
    .revision = 0,
};

__attribute__((used, section(".requests")))
static volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST_ID,
    .revision = 0,
};

__attribute__((used, section(".requests")))
static volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST_ID,
    .revision = 0,
};

__attribute__((used, section(".requests")))
static volatile struct limine_paging_mode_request paging_mode_request = {
    .id = LIMINE_PAGING_MODE_REQUEST_ID,
    .revision = 0,
    .mode = LIMINE_PAGING_MODE_X86_64_4LVL,
    .max_mode = LIMINE_PAGING_MODE_X86_64_4LVL,
    .min_mode = LIMINE_PAGING_MODE_X86_64_4LVL,
};

__attribute__((used, section(".requests")))
static volatile struct limine_mp_request mp_request = {
    .id = LIMINE_MP_REQUEST_ID,
    .revision = 0,
    .flags = LIMINE_MP_REQUEST_X86_64_X2APIC,
};

__attribute__((used, section(".requests")))
static volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST_ID,
    .revision = 0,
};

__attribute__((used, section(".requests")))
static volatile struct limine_executable_address_request exec_address_request = {
    .id = LIMINE_EXECUTABLE_ADDRESS_REQUEST_ID,
    .revision = 0,
};

__attribute__((used, section(".requests")))
static volatile struct limine_module_request module_request = {
    .id = LIMINE_MODULE_REQUEST_ID,
    .revision = 0,
};

__attribute__((used, section(".requests")))
static volatile struct limine_rsdp_request rsdp_request = {
    .id = LIMINE_RSDP_REQUEST_ID,
    .revision = 0,
};

__attribute__((used, section(".requests")))
static volatile struct limine_executable_cmdline_request cmdline_request = {
    .id = LIMINE_EXECUTABLE_CMDLINE_REQUEST_ID,
    .revision = 0,
};

__attribute__((used, section(".requests")))
static volatile struct limine_tsc_frequency_request tsc_frequency_request = {
    .id = LIMINE_TSC_FREQUENCY_REQUEST_ID,
    .revision = 0,
};

__attribute__((used, section(".requests_end_marker")))
static volatile uint64_t requests_end_marker[2] = LIMINE_REQUESTS_END_MARKER;

/* --------------------------------------------------------------------- */

void boot_requests_init(void) {
    g_boot.base_revision_ok = LIMINE_BASE_REVISION_SUPPORTED(base_revision);

    if (!g_boot.base_revision_ok) {
        /* We have no framebuffer and possibly no working HHDM yet -- serial
         * is the only channel guaranteed to work here. */
        serial_init();
        serial_puts("\r\n[FATAL] Limine did not honour our base revision "
                    "request. This bootloader is too old for Nexus.\r\n");
        hang();
    }

    if (bootloader_info_request.response != NULL) {
        g_boot.bootloader_name = bootloader_info_request.response->name;
        g_boot.bootloader_version = bootloader_info_request.response->version;
    } else {
        g_boot.bootloader_name = "unknown";
        g_boot.bootloader_version = "unknown";
    }

    if (firmware_type_request.response != NULL) {
        g_boot.firmware_type = firmware_type_request.response->firmware_type;
    } else {
        g_boot.firmware_type = LIMINE_FIRMWARE_TYPE_X86BIOS;
    }

    if (hhdm_request.response == NULL) {
        serial_init();
        serial_puts("\r\n[FATAL] No HHDM response from Limine.\r\n");
        hang();
    }
    g_boot.hhdm_offset = hhdm_request.response->offset;

    g_boot.fb = NULL;
    if (framebuffer_request.response != NULL &&
        framebuffer_request.response->framebuffer_count > 0) {
        g_boot.fb = framebuffer_request.response->framebuffers[0];
    }

    g_boot.paging_mode = paging_mode_request.response != NULL
        ? paging_mode_request.response->mode
        : LIMINE_PAGING_MODE_X86_64_4LVL;

    g_boot.mp = mp_request.response;

    if (memmap_request.response == NULL) {
        serial_init();
        serial_puts("\r\n[FATAL] No memory map response from Limine.\r\n");
        hang();
    }
    g_boot.memmap = memmap_request.response;

    if (exec_address_request.response == NULL) {
        serial_init();
        serial_puts("\r\n[FATAL] No executable address response from Limine.\r\n");
        hang();
    }
    g_boot.exec_phys_base = exec_address_request.response->physical_base;
    g_boot.exec_virt_base = exec_address_request.response->virtual_base;

    g_boot.modules = NULL;
    g_boot.module_count = 0;
    if (module_request.response != NULL) {
        g_boot.modules = module_request.response->modules;
        g_boot.module_count = module_request.response->module_count;
    }

    g_boot.rsdp = rsdp_request.response != NULL
        ? rsdp_request.response->address
        : NULL;

    g_boot.cmdline = "";
    if (cmdline_request.response != NULL &&
        cmdline_request.response->cmdline != NULL) {
        g_boot.cmdline = cmdline_request.response->cmdline;
    }

    g_boot.nosmp = strstr(g_boot.cmdline, "nosmp") != NULL;

    g_boot.tsc_freq_known = tsc_frequency_request.response != NULL;
    g_boot.tsc_freq_hz = g_boot.tsc_freq_known ? tsc_frequency_request.response->frequency : 0;
}

const struct limine_file *boot_find_module(const char *path_suffix) {
    for (uint64_t i = 0; i < g_boot.module_count; i++) {
        struct limine_file *m = g_boot.modules[i];
        if (m->path != NULL && str_has_suffix(m->path, path_suffix)) {
            return m;
        }
    }
    return NULL;
}
