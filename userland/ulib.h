#ifndef NEXUS_ULIB_H
#define NEXUS_ULIB_H

#include "task_info.h"
#include <stdbool.h>
#include <stddef.h>

/* There's no libc in a freestanding, -nostdlib userland build -- just
 * the handful of helpers these demo programs (and now nsh.c) actually
 * need. */

size_t u_strlen(const char *s);
void u_print(const char *s);
void u_putc(char c);

/* Reads one line (up to `max - 1` bytes) from stdin via SYS_read,
 * trims a trailing newline if the kernel included one, and always
 * NUL-terminates. Returns the number of characters read. As of
 * SYS_read's line-editor upgrade (cpu/syscall.c), this now echoes
 * what's typed and handles backspace -- it didn't before. */
int u_read_line(char *buf, size_t max);

void u_itoa(int val, char *buf);
int u_atoi(const char *s);

int u_getpid(void);
void u_sleep_ms(unsigned ms);
unsigned u_uptime_ms(void);

/* Minimal string helpers, added as needed -- mirrors klib/string.c's
 * kernel-side versions (same simple loop-based implementations, just
 * callable from userland where klib.h doesn't exist). */
int u_strcmp(const char *a, const char *b);
char *u_strncpy(char *dst, const char *src, size_t n);

/* Process control -- see abi/syscall_nr.h's SYS_spawn/SYS_wait. */
int u_spawn(const char *path); /* -> pid, or -1 */
int u_wait(int pid);           /* -> exit code, or -1 */

/* Filesystem -- thin wrappers over SYS_open/SYS_read/SYS_close/
 * SYS_readdir. `flags` for u_open() matches abi/syscall_nr.h's
 * O_RDONLY etc -- note the kernel currently ignores it entirely
 * (O_CREAT has never been wired up on the vfs_open() path; that's a
 * preexisting gap, not something this feature touches). */
int u_open(const char *path, int flags);
int u_read(int fd, void *buf, size_t len);
int u_close(int fd);
bool u_readdir(const char *path, unsigned index, char *name_out,
               size_t name_max);

/* Task listing -- see abi/syscall_nr.h's SYS_ps. Fills `out` with the
 * `index`'th currently-registered task (kernel or ring-3), same order
 * the kernel shell's own `ps` walks. False once `index` is past the
 * last task. */
bool u_ps(unsigned index, nx_task_info_t *out);

#endif /* NEXUS_ULIB_H */
