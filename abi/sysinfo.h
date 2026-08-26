#ifndef NEXUS_SYSINFO_H
#define NEXUS_SYSINFO_H

#include <stdint.h>

/*
 * THIS is the single canonical copy -- see abi/syscall_nr.h's header
 * comment for how `make sync-abi` mirrors it into kernel/src/abi/ and
 * userland/include/ as generated, banner-marked files.
 *
 * SYS_sysinfo (see abi/syscall_nr.h) fills this at its user-supplied
 * output pointer -- a flat, POD snapshot of a handful of read-only
 * system facts a shell wants to show a person (cpu count, x2APIC,
 * memory, heap, uptime) without a separate syscall -- or kernel-shell
 * access -- needed for each one. Deliberately flat, no pointers: this
 * crosses the user/kernel boundary via a plain copy_to_user(), same
 * convention as nx_task_info_t.
 */

typedef struct {
  uint32_t cpu_count;
  uint32_t x2apic; /* 0 or 1 -- plain uint32_t rather than a bitfield
                       or C99 bool, which has no fixed ABI width, so
                       it crosses the user/kernel boundary the same
                       predictable way every other field here does */
  uint64_t mem_total_bytes;
  uint64_t mem_used_bytes;
  uint64_t heap_used_bytes;
  uint64_t heap_capacity_bytes;
  uint64_t uptime_ms;
} nx_sysinfo_t;

#endif /* NEXUS_SYSINFO_H */
