#ifndef NEXUS_POWER_H
#define NEXUS_POWER_H

#include "klib/klib.h"

/* Shared reboot/shutdown mechanics -- both used to live entirely
 * inside shell/shell.c's cmd_reboot()/cmd_shutdown(), which was fine
 * back when the kernel shell was the only thing that could ever
 * trigger either. Now that nsh (ring-3) is the default interactive
 * shell and needs its own `reboot`/`shutdown` commands backed by real
 * syscalls (SYS_reboot/SYS_shutdown -- see cpu/syscall.c, both
 * root-gated the same way SYS_setuid is), the actual sequence --
 * stop every supervised strand, save the graph if dirty, THEN
 * actually reboot (8042 controller pulse) or shut down (ACPI \_S5,
 * falling back to a QEMU-only hack, falling back to a plain halt) --
 * needed a home neither file has to reach into the other for.
 * shell/shell.c's own cmd_reboot()/cmd_shutdown() are now thin
 * wrappers around these two. */

NORETURN void power_reboot(void);
NORETURN void power_shutdown(void);

#endif /* NEXUS_POWER_H */
