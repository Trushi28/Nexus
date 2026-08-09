#include "smp/smp.h"
#include "boot/requests.h"
#include "cpu/io.h"
#include "cpu/idt.h"
#include "mm/vmm.h"
#include "apic/lapic.h"
#include "time/timer.h"
#include "sched/sched.h"
#include "debug/log.h"
#include <limine.h>

static uint32_t online_count = 1; /* the BSP counts itself from the start */

uint32_t smp_online_cpu_count(void) {
    return __atomic_load_n(&online_count, __ATOMIC_ACQUIRE);
}

void smp_register_bsp(struct cpu_local *bsp) {
    g_cpus[0] = bsp;
    g_cpu_count = 1;
}

static void ap_entry(struct limine_mp_info *info) {
    /* Strict order: NX has to be enabled before we load page tables that
     * contain NX-marked entries (see cpu_enable_nx()'s comment), so this
     * must be the very first thing that happens here. */
    cpu_enable_nx();
    vmm_load_kernel_pagemap();

    struct cpu_local *cpu = (struct cpu_local *)info->extra_argument;
    cpu_setup_current(cpu);
    idt_load();

    lapic_init();
    timer_start_periodic_for_this_cpu();

    cpu->online = true;
    __atomic_fetch_add(&online_count, 1, __ATOMIC_RELEASE);

    kprintf("[smp] CPU #%u online (LAPIC ID %u, x2APIC %s)\n",
            cpu->cpu_index, cpu->lapic_id, lapic_using_x2apic() ? "yes" : "no");

    sched_enter_idle();
}

void smp_init(void) {
    struct limine_mp_response *mp = g_boot.mp;

    if (g_boot.nosmp) {
        kprintf("[smp] \"nosmp\" on the command line -- staying uniprocessor\n");
        return;
    }
    if (mp == NULL || mp->cpu_count <= 1) {
        kprintf("[smp] no other CPUs reported -- staying uniprocessor\n");
        return;
    }

    kprintf("[smp] %lu CPU(s) reported by the bootloader, x2APIC %s; "
            "bringing up application processors...\n",
            mp->cpu_count, (mp->flags & 1) ? "enabled" : "not available");

    for (uint64_t i = 0; i < mp->cpu_count; i++) {
        struct limine_mp_info *info = mp->cpus[i];
        if (info->lapic_id == mp->bsp_lapic_id) {
            continue; /* that's the BSP -- already running, nothing to release */
        }

        uint32_t idx = g_cpu_count;
        if (idx >= MAX_CPUS) {
            kprintf("[smp] MAX_CPUS reached, ignoring the remaining CPUs\n");
            break;
        }

        struct cpu_local *cpu = cpu_local_create(idx, info->lapic_id, info->processor_id, false);
        g_cpus[idx] = cpu;
        g_cpu_count++;

        info->extra_argument = (uint64_t)cpu;
        __atomic_store_n(&info->goto_address, ap_entry, __ATOMIC_RELEASE);
    }

    kprintf("[smp] release signalled to %u application processor(s); "
            "they will check in asynchronously\n", g_cpu_count - 1);
}
