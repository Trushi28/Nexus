#ifndef NEXUS_CPU_H
#define NEXUS_CPU_H

#include "klib/klib.h"
#include "cpu/gdt.h"

#define MAX_CPUS 256

struct task; /* sched/sched.h */

struct cpu_local {
    struct cpu_local *self; /* must be the first field -- see this_cpu() */

    uint32_t cpu_index;     /* our own 0..N-1 numbering */
    uint32_t lapic_id;
    uint32_t processor_id;  /* Limine/ACPI processor id */
    bool     is_bsp;
    volatile bool online;

    struct gdt_table gdt;
    struct tss        tss;
    uint8_t  df_stack[4096] ALIGNED(16); /* #DF / NMI emergency stack (IST1) */

    struct task *current_task;
    struct task *idle_task;

    uint64_t tick_count;    /* incremented on every local timer interrupt */
    uint32_t quantum_ticks; /* scheduler: ticks since this CPU's last reschedule */

    uint64_t sched_stack_top;
};

extern struct cpu_local *g_cpus[MAX_CPUS];
extern uint32_t g_cpu_count;

static inline struct cpu_local *this_cpu(void) {
    struct cpu_local *p;
    asm volatile ("mov %%gs:0, %0" : "=r"(p));
    return p;
}

/* Sets EFER.NXE. Must be called on every CPU before that CPU's CR3 can
 * point at page tables containing any NX-marked entry -- see cpu.c for
 * why this is not optional. */
void cpu_enable_nx(void);

/* Allocates and zero-initialises a new cpu_local block (does not set up
 * its GDT/TSS -- see cpu_setup_current(), which is what installs GS_BASE
 * too). Safe to call before the heap exists (backed directly by PMM
 * pages, not kmalloc). */
struct cpu_local *cpu_local_create(uint32_t cpu_index, uint32_t lapic_id, uint32_t processor_id, bool is_bsp);

/* Loads this CPU's GDT/TSS/IDT, sets GS_BASE to `cpu`, and enables the
 * NX bit and write-protect in CR4/EFER as appropriate. Call once per
 * CPU, right after the IDT has been built (BSP) or loaded (APs). */
void cpu_setup_current(struct cpu_local *cpu);

#endif /* NEXUS_CPU_H */
