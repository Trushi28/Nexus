#include "cpu/idt.h"
#include "cpu/isr.h"
#include "cpu/gdt.h"
#include "cpu/vectors.h"

struct PACKED idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
};

struct PACKED idtr {
    uint16_t limit;
    uint64_t base;
};

static struct idt_entry idt[256];
static struct idtr idtr_ptr;

/* present, 64-bit interrupt gate; DPL lives in bits 5:6 and is filled
 * in per-vector by set_gate() below -- every vector is DPL0 (kernel
 * only) except VEC_SYSCALL, which needs DPL3 so ring-3 code's
 * `int $0x80` doesn't take a #GP just for trying. */
#define IDT_TYPE_INTERRUPT_GATE 0x8E

static void set_gate(int vector, void *handler, uint8_t ist, uint8_t dpl) {
    uint64_t addr = (uint64_t)handler;
    idt[vector].offset_low  = (uint16_t)(addr & 0xFFFF);
    idt[vector].selector    = GDT_KERNEL_CODE;
    idt[vector].ist         = ist;
    idt[vector].type_attr   = IDT_TYPE_INTERRUPT_GATE | ((dpl & 0x3) << 5);
    idt[vector].offset_mid  = (uint16_t)((addr >> 16) & 0xFFFF);
    idt[vector].offset_high = (uint32_t)((addr >> 32) & 0xFFFFFFFF);
    idt[vector].reserved    = 0;
}

void idt_init(void) {
    for (int v = 0; v < 256; v++) {
        uint8_t ist = (v == 8) ? 1 : 0; /* #DF always runs on the IST1 emergency stack */
        uint8_t dpl = (v == VEC_SYSCALL) ? 3 : 0;
        set_gate(v, isr_stub_table[v], ist, dpl);
    }

    idtr_ptr.limit = sizeof(idt) - 1;
    idtr_ptr.base  = (uint64_t)idt;

    idt_load();
}

void idt_load(void) {
    asm volatile ("lidt %0" : : "m"(idtr_ptr));
}
