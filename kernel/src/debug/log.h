#ifndef NEXUS_LOG_H
#define NEXUS_LOG_H

#include "klib/klib.h"

/* kprintf writes to both the serial port and the framebuffer console (if
 * one exists). This is the kernel's one and only logging/print facility;
 * everything from boot banners to the interactive shell goes through it. */
__attribute__((format(printf, 1, 2)))
int kprintf(const char *fmt, ...);

#endif /* NEXUS_LOG_H */
