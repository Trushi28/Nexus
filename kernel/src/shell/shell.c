#include "shell/shell.h"
#include "klib/klib.h"
#include "debug/log.h"
#include "debug/serial.h"
#include "video/console.h"
#include "video/fb.h"
#include "drivers/keyboard.h"
#include "drivers/pci.h"
#include "mm/pmm.h"
#include "mm/heap.h"
#include "cpu/cpu.h"
#include "cpu/io.h"
#include "apic/lapic.h"
#include "time/timer.h"
#include "sched/sched.h"
#include "boot/requests.h"
#include "fs/vfs.h"
#include "proc/process.h"

#define LINE_MAX 200

static void print_prompt(void) {
    kprintf("nexus> ");
}

static void echo_backspace(void) {
    serial_puts("\b \b");
    console_backspace();
}

static uint32_t read_line(char *buf, uint32_t max) {
    uint32_t len = 0;
    for (;;) {
        char c = keyboard_getc();

        if (c == '\n') {
            kprintf("\n");
            buf[len] = '\0';
            return len;
        }

        if (c == '\b') {
            if (len > 0) {
                len--;
                echo_backspace();
            }
            continue;
        }

        if (len + 1 < max && c >= 0x20 && c < 0x7F) {
            buf[len++] = c;
            kprintf("%c", c);
        }
    }
}

static const char *skip_spaces(const char *s) {
    while (*s == ' ') {
        s++;
    }
    return s;
}

static void fmt_bytes(char *out, size_t out_len, uint64_t bytes) {
    if (bytes >= 1024ULL * 1024 * 1024) {
        ksnprintf(out, out_len, "%lu MiB", bytes / (1024 * 1024));
    } else if (bytes >= 1024ULL * 1024) {
        ksnprintf(out, out_len, "%lu KiB", bytes / 1024);
    } else {
        ksnprintf(out, out_len, "%lu B", bytes);
    }
}

static void cmd_help(void) {
    kprintf(
        "Nexus shell -- available commands:\n"
        "  help              this text\n"
        "  clear             clear the screen\n"
        "  echo <text>       print text back\n"
        "  meminfo           physical memory + heap usage\n"
        "  cpuinfo           list online CPUs and APIC mode\n"
        "  uptime            time since boot\n"
        "  ps                list every live task\n"
        "  lspci             enumerate PCI devices\n"
        "  uname             kernel/bootloader version info\n"
        "  ls [path]         list a directory (default: /)\n"
        "  cat <path>        print a file's contents\n"
        "  run <path>        load and run an ELF binary in ring 3\n"
        "  matrix            you know the one -- any key to stop\n"
        "  reboot            reset the machine (8042 controller pulse)\n"
    );
}

static void cmd_meminfo(void) {
    char total[32], used[32], free_[32], hused[32], hcap[32];
    fmt_bytes(total, sizeof(total), pmm_total_bytes());
    fmt_bytes(used, sizeof(used), pmm_used_bytes());
    fmt_bytes(free_, sizeof(free_), pmm_free_bytes());
    fmt_bytes(hused, sizeof(hused), heap_used_bytes());
    fmt_bytes(hcap, sizeof(hcap), heap_capacity_bytes());

    kprintf("physical: %s total, %s used, %s free\n", total, used, free_);
    kprintf("heap:     %s used, %s reserved\n", hused, hcap);
}

static void cmd_cpuinfo(void) {
    kprintf("%u CPU(s) known, x2APIC %s\n", g_cpu_count,
            lapic_using_x2apic() ? "enabled" : "not in use");
    for (uint32_t i = 0; i < g_cpu_count; i++) {
        struct cpu_local *c = g_cpus[i];
        kprintf("  cpu%u: lapic_id=%u %s %s\n", c->cpu_index, c->lapic_id,
                c->is_bsp ? "(BSP)" : "(AP) ",
                c->online || c->is_bsp ? "online" : "offline");
    }
}

static void cmd_uptime(void) {
    uint64_t ms = timer_uptime_ms();
    uint64_t s = ms / 1000;
    kprintf("up %lu:%02lu:%02lu.%03lu\n", s / 3600, (s / 60) % 60, s % 60, ms % 1000);
}

static void cmd_lspci(void) {
    pci_scan();
    uint32_t n = pci_device_count();
    kprintf("%u device(s):\n", n);
    for (uint32_t i = 0; i < n; i++) {
        const struct pci_device *d = pci_device_at(i);
        kprintf("  %02x:%02x.%x  %04x:%04x  %s\n",
                d->bus, d->slot, d->func, d->vendor_id, d->device_id,
                pci_class_name(d->class_code));
    }
}

static void cmd_uname(void) {
    kprintf("Nexus OS -- x86-64, SMP, x2APIC\n");
    kprintf("bootloader: %s %s\n", g_boot.bootloader_name, g_boot.bootloader_version);
    kprintf("firmware:   %s\n",
            g_boot.firmware_type == LIMINE_FIRMWARE_TYPE_X86BIOS ? "BIOS" :
            g_boot.firmware_type == LIMINE_FIRMWARE_TYPE_EFI32   ? "UEFI (32-bit)" :
            g_boot.firmware_type == LIMINE_FIRMWARE_TYPE_EFI64   ? "UEFI (64-bit)" : "unknown");
}

static void cmd_reboot(void) {
    kprintf("rebooting...\n");
    timer_busy_wait_ms(50);
    /* Pulse the 8042 keyboard controller's reset line -- the standard,
     * near-universally-supported software reset trick on PC-compatible
     * hardware. */
    uint8_t status;
    do {
        status = inb(0x64);
    } while (status & 0x02);
    outb(0x64, 0xFE);
    hang(); /* if we're still here, the controller didn't cooperate */
}

static void print_left(const char *s, int width) {
    int n = 0;
    for (const char *p = s; *p; p++) {
        n++;
    }
    kprintf("%s", s);
    for (int i = n; i < width; i++) {
        kprintf(" ");
    }
}

static void ps_print_one(struct task *t, void *arg) {
    (void)arg;
    static const char *state_names[] = {
        "ready", "running", "sleeping", "blocked", "dead",
    };
    kprintf("  %4lu  ", t->id);
    print_left(t->name, 18);
    print_left(state_names[t->state], 11);
    kprintf("%s\n", t->is_user ? "user" : "kernel");
}

static void cmd_ps(void) {
    kprintf("  %4s  ", "PID");
    print_left("NAME", 18);
    print_left("STATE", 11);
    kprintf("%s\n", "RING");
    sched_for_each_task(ps_print_one, NULL);
}

static void cmd_ls(const char *path) {
    if (*path == '\0') {
        path = "/";
    }
    char name[64];
    uint32_t i = 0;
    bool any = false;
    while (vfs_readdir(path, i, name, sizeof(name))) {
        kprintf("  %s\n", name);
        i++;
        any = true;
    }
    if (!any) {
        kprintf("(empty, or no such directory)\n");
    }
}

static void cmd_cat(const char *path) {
    if (*path == '\0') {
        kprintf("usage: cat <path>\n");
        return;
    }
    struct vfs_file *f;
    if (!vfs_open(path, &f)) {
        kprintf("cat: %s: no such file\n", path);
        return;
    }
    char buf[257];
    size_t n;
    while ((n = vfs_read(f, buf, sizeof(buf) - 1)) > 0) {
        buf[n] = '\0';
        kprintf("%s", buf);
    }
    kprintf("\n");
    vfs_close(f);
}

static void cmd_run(const char *path) {
    if (*path == '\0') {
        kprintf("usage: run <path>\n");
        return;
    }
    struct task *t = process_spawn(path, path);
    if (t == NULL) {
        return; /* process_spawn already explained why */
    }
    int code = process_wait(t);
    kprintf("[%s exited with code %d]\n", path, code);
}

#define MATRIX_COLS_MAX 256

static void cmd_matrix(void) {
    if (!fb_available()) {
        kprintf("matrix: no framebuffer available\n");
        return;
    }

    uint32_t cols = console_cols();
    uint32_t rows = console_rows();
    if (cols == 0 || rows == 0) {
        return;
    }
    if (cols > MATRIX_COLS_MAX) {
        cols = MATRIX_COLS_MAX;
    }

    keyboard_flush();
    kprintf("(matrix rain -- press any key to stop)\n");

    int drop_row[MATRIX_COLS_MAX];
    uint32_t rng = (uint32_t)(timer_uptime_ms() * 2654435761ULL) | 1u;
    for (uint32_t c = 0; c < cols; c++) {
        rng = rng * 1103515245u + 12345u;
        drop_row[c] = -(int)(rng % rows);
    }

    console_set_colors(0x0033FF55, 0x00050805);

    while (!keyboard_haskey()) {
        for (uint32_t c = 0; c < cols; c++) {
            rng = rng * 1103515245u + 12345u;
            char glyph = (char)(0x21 + (rng >> 16) % (0x7E - 0x21));

            int r = drop_row[c];
            if (r >= 0 && (uint32_t)r < rows) {
                console_putc_at(c, (uint32_t)r, glyph);
            }
            int tail = r - 1;
            if (tail >= 0 && (uint32_t)tail < rows) {
                console_putc_at(c, (uint32_t)tail, ' ');
            }

            drop_row[c] = r + 1;
            rng = rng * 1103515245u + 12345u;
            if (drop_row[c] > (int)(rows + rng % rows)) {
                drop_row[c] = -(int)(rng % rows);
            }
        }
        sched_sleep_ms(45);
    }

    keyboard_flush();
    console_set_colors(0x00E0E0E0, 0x000B0E14);
    console_clear();
}

static void dispatch(char *line) {
    const char *cmd = skip_spaces(line);
    if (*cmd == '\0') {
        return;
    }

    if (strcmp(cmd, "help") == 0) {
        cmd_help();
    } else if (strcmp(cmd, "clear") == 0) {
        console_clear();
    } else if (str_has_prefix(cmd, "echo")) {
        const char *rest = skip_spaces(cmd + 4);
        kprintf("%s\n", rest);
    } else if (strcmp(cmd, "meminfo") == 0) {
        cmd_meminfo();
    } else if (strcmp(cmd, "cpuinfo") == 0) {
        cmd_cpuinfo();
    } else if (strcmp(cmd, "uptime") == 0) {
        cmd_uptime();
    } else if (strcmp(cmd, "ps") == 0) {
        cmd_ps();
    } else if (strcmp(cmd, "lspci") == 0) {
        cmd_lspci();
    } else if (strcmp(cmd, "uname") == 0) {
        cmd_uname();
    } else if (strcmp(cmd, "ls") == 0) {
        cmd_ls("");
    } else if (str_has_prefix(cmd, "ls ")) {
        cmd_ls(skip_spaces(cmd + 3));
    } else if (str_has_prefix(cmd, "cat ")) {
        cmd_cat(skip_spaces(cmd + 4));
    } else if (str_has_prefix(cmd, "run ")) {
        cmd_run(skip_spaces(cmd + 4));
    } else if (strcmp(cmd, "matrix") == 0) {
        cmd_matrix();
    } else if (strcmp(cmd, "reboot") == 0) {
        cmd_reboot();
    } else {
        kprintf("unknown command: %s (try 'help')\n", cmd);
    }
}

void shell_task(void *arg) {
    (void)arg;
    char line[LINE_MAX];

    kprintf("\nWelcome to Nexus. Type 'help' for a list of commands.\n");

    for (;;) {
        print_prompt();
        read_line(line, sizeof(line));
        dispatch(line);
    }
}
