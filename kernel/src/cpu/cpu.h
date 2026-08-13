#ifndef NEXUS_CPU_H
#define NEXUS_CPU_H

#include "cpu/gdt.h"
#include "klib/klib.h"

#define MAX_CPUS 256

struct task; /* sched/sched.h */

struct cpu_local {
  struct cpu_local *self; /* must be the first field -- see this_cpu() */

  uint32_t cpu_index; /* our own 0..N-1 numbering */
  uint32_t lapic_id;
  uint32_t processor_id; /* Limine/ACPI processor id */
  bool is_bsp;
  volatile bool online;

  struct gdt_table gdt;
  struct tss tss;
  uint8_t df_stack[4096] ALIGNED(16); /* #DF / NMI emergency stack (IST1) */

  struct task *current_task;
  struct task *idle_task;
  struct task *pending_exit;
  struct task *parked_head;
  uint64_t tick_count; /* incremented on every local timer interrupt */
  uint32_t
      quantum_ticks; /* scheduler: ticks since this CPU's last reschedule */

  uint64_t sched_stack_top;
};

extern struct cpu_local *g_cpus[MAX_CPUS];
extern uint32_t g_cpu_count;

static inline struct cpu_local *this_cpu(void) {
  struct cpu_local *p;
  asm volatile("mov %%gs:0, %0" : "=r"(p));
  return p;
}

void cpu_enable_nx(void);

struct cpu_local *cpu_local_create(uint32_t cpu_index, uint32_t lapic_id,
                                   uint32_t processor_id, bool is_bsp);

void cpu_setup_current(struct cpu_local *cpu);

#endif /* NEXUS_CPU_H */
