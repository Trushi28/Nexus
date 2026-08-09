#ifndef NEXUS_ULIB_H
#define NEXUS_ULIB_H

#include <stddef.h>

/* There's no libc in a freestanding, -nostdlib userland build -- just
 * the handful of helpers these three demo programs actually need. */

size_t   u_strlen(const char *s);
void     u_print(const char *s);
void     u_putc(char c);

/* Reads one line (up to `max - 1` bytes) from stdin via SYS_read,
 * trims a trailing newline if the kernel included one, and always
 * NUL-terminates. Returns the number of characters read. */
int      u_read_line(char *buf, size_t max);

void     u_itoa(int val, char *buf);
int      u_atoi(const char *s);

int      u_getpid(void);
void     u_sleep_ms(unsigned ms);
unsigned u_uptime_ms(void);

#endif /* NEXUS_ULIB_H */
