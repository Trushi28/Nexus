#ifndef NEXUS_ISR_H
#define NEXUS_ISR_H

#include "klib/klib.h"

/* Matches exactly what isr_stubs.S pushes, low address (top of stack,
 * where RDI points on entry to isr_common_handler) to high address. */
struct PACKED interrupt_frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector;
    uint64_t error_code;
    /* CPU-pushed, always all five in long mode (see isr_stubs.S). */
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
};

typedef void (*interrupt_handler_t)(struct interrupt_frame *frame);

/* Extern so idt.c can point each gate at the right generated stub. */
extern void *isr_stub_table[256];

/* Installs a C handler for `vector` (32-255; 0-31 are the CPU exceptions
 * and already have a default handler installed by isr_init()). Only one
 * handler per vector -- IRQ sharing is not implemented in v1. */
void register_interrupt_handler(uint8_t vector, interrupt_handler_t handler);

void isr_init(void);

#endif /* NEXUS_ISR_H */
