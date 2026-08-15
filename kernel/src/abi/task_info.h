#ifndef NEXUS_TASK_INFO_H
#define NEXUS_TASK_INFO_H

#include <stdint.h>

/*
 * !!! THIS FILE IS MIRRORED VERBATIM to userland/include/task_info.h,
 * exactly like abi/syscall_nr.h -- see that file's header comment for
 * why (no shared sysroot between the two build trees). If you change
 * one, change the other.
 *
 * The record SYS_ps (see abi/syscall_nr.h) fills at its user-supplied
 * output pointer -- one call, one task, in registry order (same order
 * `ps` in the kernel shell walks). Deliberately flat/POD, fixed
 * width, no pointers: this crosses the user/kernel boundary via a
 * plain copy_to_user(), so anything here has to be safe to blit
 * byte-for-byte.
 */

#define NX_TASK_NAME_MAX 32

/* Mirrors enum task_state in sched/sched.h numerically (TASK_READY=0
 * .. TASK_DEAD=5) -- kept as a plain uint32_t rather than including
 * sched.h here, since this header has to compile standalone in
 * userland where sched.h doesn't exist. No compile-time link between
 * the two -- if sched.h's enum order ever changes, this silently goes
 * stale, same caveat as syscall_nr.h being a hand-kept copy. */
typedef struct {
  uint64_t pid;
  char name[NX_TASK_NAME_MAX];
  uint32_t state;
  uint32_t is_user;
} nx_task_info_t;

#endif /* NEXUS_TASK_INFO_H */
