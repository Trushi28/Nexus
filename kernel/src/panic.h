#ifndef NEXUS_PANIC_H
#define NEXUS_PANIC_H

#include "klib/klib.h"

/* Registers the cross-CPU halt IPI handler. Call once, early, from the BSP. */
void panic_init(void);

/* Prints a banner + message to serial and the framebuffer console, then
 * asks every other CPU to halt too, and never returns. */
NORETURN void panic(const char *fmt, ...);

#endif /* NEXUS_PANIC_H */
