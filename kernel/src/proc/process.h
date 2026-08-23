#ifndef NEXUS_PROCESS_H
#define NEXUS_PROCESS_H

#include "klib/klib.h"

struct task; /* sched/sched.h */

/* Where every process's user stack lives (top, growing down) and how
 * big it is. One fixed address is fine -- v1 never runs more than one
 * ring-3 task built from the same address space, and every process
 * gets its own address space, so there's no collision to worry about. */
#define USER_STACK_TOP 0x0000700000000000ULL
#define USER_STACK_PAGES 8 /* 32 KiB */

/* sys_brk()'s ceiling, in pages, above wherever the loaded ELF image
 * ends -- a hard cap rather than "grow forever", just to keep a
 * runaway process from quietly eating all of physical memory. 1MiB is
 * plenty for the demo programs in userland/. */
#define USER_BRK_MAX_PAGES 256

/* Loads the ELF64 executable at `path` from the VFS into a fresh
 * per-process address space and creates a ready-to-run ring-3 task for
 * it (see task_create_user()). `name` is just the task's display name
 * (for `ps`) -- doesn't need to match `path`. `uid` is the clearance
 * the new process starts with -- the caller decides this explicitly
 * rather than getting a default, since "what should this be" depends
 * entirely on who's calling: the kernel shell's `run` always passes 0
 * (it's trusted/root itself), `runas` passes whatever it was given,
 * and SYS_spawn's handler (cpu/syscall.c) passes the calling ring-3
 * task's OWN current uid, matching an ordinary fork/exec's default of
 * inheriting rather than escalating. Returns NULL, having already
 * logged why, on any failure: a bad path, a malformed ELF, or
 * out-of-memory. Cleans up fully on failure -- never leaks a
 * half-built address space. */
struct task *process_spawn(const char *path, const char *name, uint32_t uid);

/* Blocks the calling task until `child` exits, then returns its exit
 * code. Thin wrapper over sched_wait_task() -- see that function's
 * comment for the one-call-per-child rule. */
int process_wait(struct task *child);

#endif /* NEXUS_PROCESS_H */
