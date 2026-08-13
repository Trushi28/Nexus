#ifndef NEXUS_VECTORS_H
#define NEXUS_VECTORS_H

/*
 * 0-31   : CPU exceptions (fixed by the architecture)
 * 32-63  : device IRQs, routed through the I/O APIC
 * 64     : per-CPU LAPIC timer tick
 * 0x80   : syscall gate (int $0x80) -- the only vector in the IDT with
 *          DPL=3, i.e. the only one ring-3 code is allowed to invoke
 *          directly. See idt_init()'s dpl-per-vector special case.
 * 0xFB   : reschedule IPI (wake an idle/sleeping CPU)
 * 0xFC   : TLB shootdown IPI
 * 0xFE   : LAPIC internal error
 * 0xFF   : spurious (required to end in 0xF by the APIC spec; 0xFF is
 *          the conventional choice)
 */

#define VEC_IRQ_BASE 32
#define VEC_KEYBOARD (VEC_IRQ_BASE + 1) /* ISA IRQ1 */

#define VEC_TIMER 0x40

#define VEC_SYSCALL 0x80

#define VEC_RESCHEDULE_IPI 0xFB
#define VEC_TLB_SHOOTDOWN_IPI 0xFC
#define VEC_PANIC_HALT_IPI 0xFD
#define VEC_LAPIC_ERROR 0xFE
#define VEC_SPURIOUS 0xFF
#define VEC_TIMER 0x40
#define VEC_NVME_ADMIN 0x41
#define VEC_NVME_IO 0x42

#endif /* NEXUS_VECTORS_H */
