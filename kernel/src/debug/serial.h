#ifndef NEXUS_SERIAL_H
#define NEXUS_SERIAL_H

#include "klib/klib.h"

/* Initialise COM1 (0x3F8) at 115200 8N1. Safe to call before anything
 * else exists -- serial is our earliest and most reliable debug channel,
 * it works even if the framebuffer request failed or paging is broken. */
void serial_init(void);

void serial_putc(char c);
void serial_puts(const char *s);

#endif /* NEXUS_SERIAL_H */
