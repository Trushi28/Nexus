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
unsigned u_getuid(void);
int u_setuid(unsigned uid);
void u_sleep_ms(unsigned ms);
unsigned u_uptime_ms(void);

/* Minimal string helpers, added as needed -- mirrors klib/string.c's
 * kernel-side versions (same simple loop-based implementations, just
 * callable from userland where klib.h doesn't exist). */
int u_strcmp(const char *a, const char *b);
char *u_strncpy(char *dst, const char *src, size_t n);
void u_print_left(const char *s, int width);

/* Process control -- see abi/syscall_nr.h's SYS_spawn/SYS_wait. */
int u_spawn(const char *path); /* -> pid, or -1 */
int u_wait(int pid);           /* -> exit code, or -1 */

int u_wait_any(int *code_out);
/* Replaces the calling process's own image with the ELF at `path`, in
 * place: same pid, same open fds, fresh address space and entry
 * point (see abi/syscall_nr.h's SYS_exec). Never returns on success --
 * there's no call site left to return to, exactly like a real
 * exec(). Returns -1 on failure, in which case the caller's current
 * image is left completely untouched. */
int u_exec(const char *path);

/* Entry-point signature for u_split()'s child -- deliberately shaped
 * like the KERNEL's own task_entry_t (sched/sched.h), not like
 * main()'s int-returning, no-args C convention: split() is Nexus's
 * take on "give me a second running copy of my current process," and
 * its child starts at an explicit function + argument rather than
 * resuming wherever the caller was (see u_split()'s own comment for
 * why that's the whole point). */
typedef void (*u_task_entry_t)(void *arg);

/* Duplicates the calling process's ENTIRE current memory image (heap,
 * globals, stack contents, everything -- a real copy, not
 * copy-on-write) into a brand new, independently scheduled task, and
 * returns its pid to the ORIGINAL caller, which keeps running
 * completely unaffected -- there's no POSIX-style double return here.
 * The new task does NOT resume wherever u_split() was called from; it
 * starts fresh at `entry(arg)`, on its own stack, and exits
 * automatically (code 0) if `entry` ever returns. Every fd the caller
 * had open is independently duplicated into the child (its own
 * offset, not shared with the parent's -- see fs/vfs.c's vfs_dup() on
 * the kernel side). Returns the child's pid, or -1 on failure (in
 * which case nothing happens: no child, caller's own state
 * untouched). */
int u_split(u_task_entry_t entry, void *arg);

int u_open(const char *path, int flags);
int u_read(int fd, void *buf, size_t len);
int u_write(int fd, const void *buf, size_t len);
int u_close(int fd);
bool u_readdir(const char *path, unsigned index, char *name_out,
               size_t name_max);
int u_kill(int pid); /* -> 0, or -1 */

/* Linear scan over u_ps() until `pid` matches -- there's no
 * find-by-pid syscall, SYS_ps only takes a registry index, so this is
 * userland's equivalent of sched_find_waitable_task() for code that
 * only has a pid (nsh's background-job table uses this to poll "is it
 * done yet?"). O(live task count) per call; fine at this scale. */
bool u_find_task(int pid, nx_task_info_t *out);
/* Task listing -- see abi/syscall_nr.h's SYS_ps. Fills `out` with the
 * `index`'th currently-registered task (kernel or ring-3), same order
 * the kernel shell's own `ps` walks. False once `index` is past the
 * last task. */
bool u_ps(unsigned index, nx_task_info_t *out);

#endif /* NEXUS_ULIB_H */
