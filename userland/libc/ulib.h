#ifndef NEXUS_ULIB_H
#define NEXUS_ULIB_H

#include "sysinfo.h"
#include "task_info.h"
#include <stdbool.h>
#include <stddef.h>

size_t u_strlen(const char *s);
void u_print(const char *s);
void u_putc(char c);

/* Reads up to `max - 1` characters from stdin and NUL-terminates `buf`.
 * A trailing newline is removed. */
int u_read_line(char *buf, size_t max);

void u_itoa(int val, char *buf);
int u_atoi(const char *s);

int u_getpid(void);
unsigned u_getuid(void);
int u_setuid(unsigned uid);
void u_sleep_ms(unsigned ms);
unsigned u_uptime_ms(void);

int u_strcmp(const char *a, const char *b);
char *u_strncpy(char *dst, const char *src, size_t n);
void u_print_left(const char *s, int width);

int u_spawn(const char *path); /* -> pid, or -1 */
int u_wait(int pid);           /* -> exit code, or -1 */

int u_wait_any(int *code_out);

/*
 * Replaces the calling process image with the ELF at `path`.
 * Keeps the same pid and open fds, but does not return on success.
 * Returns -1 on failure without changing the current process.
 */
int u_exec(const char *path);

/* Entry function used by the child created with u_split(). */
typedef void (*u_task_entry_t)(void *arg);

/*
 * Creates a new task with a copy of the caller's current memory and file
 * descriptors. The child starts at `entry(arg)` instead of resuming here.
 *
 * Returns the child's pid to the caller, or -1 on failure.
 */
int u_split(u_task_entry_t entry, void *arg);

int u_open(const char *path, int flags);
int u_read(int fd, void *buf, size_t len);
int u_write(int fd, const void *buf, size_t len);
int u_close(int fd);
bool u_readdir(const char *path, unsigned index, char *name_out,
               size_t name_max);
int u_kill(int pid); /* -> 0, or -1 */

/* Finds the currently registered task with the given pid. */
bool u_find_task(int pid, nx_task_info_t *out);
bool u_ps(unsigned index, nx_task_info_t *out);

int u_chdir(const char *path);        /* -> 0, or -1 */
bool u_getcwd(char *buf, size_t max); /* -> true and fills buf, or false */

int u_reboot(void);
int u_shutdown(void);

/* A flat snapshot of cpu count/x2APIC/memory/heap/uptime -- see
 * abi/sysinfo.h's nx_sysinfo_t. Available to any uid. */
bool u_sysinfo(nx_sysinfo_t *out);

int u_gsync(void); /* -> 0, or -1 */
int u_gload(void); /* -> 0, or -1 */
void u_color_accent(void);
void u_color_dim(void);
void u_color_error(void);
void u_color_reset(void);

#endif /* NEXUS_ULIB_H */
