#ifndef NEXUS_PROCESS_H
#define NEXUS_PROCESS_H

#include "klib/klib.h"

struct task;

// Fixed user stack location, growing downward.
#define USER_STACK_TOP 0x0000700000000000ULL
#define USER_STACK_PAGES 8 /* 32 KiB */

// Maximum heap size above the loaded ELF image.
#define USER_BRK_MAX_PAGES 256

/*
 * Loads an ELF executable into a new address space and creates a
 * ready-to-run user task.
 *
 * `uid` specifies the new task's initial uid. Returns NULL on failure.
 */
struct task *process_spawn(const char *path, const char *name, uint32_t uid);

// Blocks until `child` exits and returns its exit code.
int process_wait(struct task *child);

#endif /* NEXUS_PROCESS_H */
