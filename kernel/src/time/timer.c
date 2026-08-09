#include "time/timer.h"
#include "time/pit.h"
#include "apic/lapic.h"
#include "cpu/isr.h"
#include "cpu/vectors.h"
#include "cpu/cpu.h"
#include "debug/log.h"
#include "sched/sched.h"

#define TIMER_DIVIDE_BY_16 0x3

static uint32_t apic_ticks_per_ms = 0;
static volatile uint64_t bsp_ticks = 0;

static void timer_isr(struct interrupt_frame *frame) {
    (void)frame;
    struct cpu_local *cpu = this_cpu();
    cpu->tick_count++;
    if (cpu->is_bsp) {
        bsp_ticks++;
    }
    sched_tick();
}

void timer_calibrate(void) {
    register_interrupt_handler(VEC_TIMER, timer_isr);

    lapic_timer_set_divide(TIMER_DIVIDE_BY_16);
    lapic_timer_start_oneshot_masked(0xFFFFFFFF);

    const uint32_t window_ms = 20;
    pit_wait_ms(window_ms);

    uint32_t remaining = lapic_timer_current_count();
    lapic_timer_stop();

    uint32_t elapsed = 0xFFFFFFFF - remaining;
    apic_ticks_per_ms = elapsed / window_ms;

    if (apic_ticks_per_ms == 0) {
        apic_ticks_per_ms = 1; /* pathological/virtualised timer; keep going anyway */
    }

    kprintf("[timer] LAPIC timer calibrated: %u ticks/ms (divide-by-16)\n",
            apic_ticks_per_ms);
}

void timer_start_periodic_for_this_cpu(void) {
    lapic_timer_set_divide(TIMER_DIVIDE_BY_16);
    uint32_t count = apic_ticks_per_ms * (1000 / TIMER_HZ);
    if (count == 0) {
        count = 1;
    }
    lapic_timer_start_periodic(VEC_TIMER, count);
}

uint64_t timer_uptime_ms(void) {
    return bsp_ticks * (1000 / TIMER_HZ);
}

void timer_busy_wait_ms(uint32_t ms) {
    uint64_t target = timer_uptime_ms() + ms;
    while (timer_uptime_ms() < target) {
        asm volatile ("pause");
    }
}
