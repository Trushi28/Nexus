#ifndef NEXUS_VECTORS_H
#define NEXUS_VECTORS_H

/*
 * 0-31   : CPU exceptions
 * 32-63  : device IRQs
 * 0x40   : per-CPU LAPIC timer
 * 0x80   : syscall gate
 * 0xFB   : reschedule IPI
 * 0xFC   : TLB shootdown IPI
 * 0xFE   : LAPIC error
 * 0xFF   : spurious
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
#define VEC_NVME_ADMIN 0x41
#define VEC_NVME_IO 0x42

#endif /* NEXUS_VECTORS_H */
