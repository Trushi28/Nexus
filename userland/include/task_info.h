#ifndef NEXUS_TASK_INFO_H
#define NEXUS_TASK_INFO_H

#include <stdint.h>

/*
 * !!! THIS IS A COPY !!!
 * Canonical source: kernel/src/abi/task_info.h -- kept identical here
 * because userland and the kernel are separate build trees with no
 * shared installed sysroot. If you change one, change the other.
 *
 * See the canonical copy for the full comment on what this is.
 */

#define NX_TASK_NAME_MAX 32

typedef struct {
  uint64_t pid;
  char name[NX_TASK_NAME_MAX];
  uint32_t state;
  uint32_t is_user;
} nx_task_info_t;

#endif /* NEXUS_TASK_INFO_H */
