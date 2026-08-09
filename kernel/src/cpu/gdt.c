#include "cpu/gdt.h"

static void set_entry(struct gdt_entry *e, uint8_t access, uint8_t gran) {
    /* Base and limit are meaningless for long-mode code/data descriptors
     * (the CPU treats them as flat/0-based), but must still be filled
     * with *something* valid or some CPUs will refuse to load them. */
    e->limit_low   = 0xFFFF;
    e->base_low    = 0;
    e->base_mid    = 0;
    e->access      = access;
    e->granularity = gran;
    e->base_high   = 0;
}

static void set_tss_descriptor(struct gdt_table *table, struct tss *tss) {
    uint64_t base = (uint64_t)tss;
    uint32_t limit = sizeof(struct tss) - 1;

    struct gdt_entry *lo = &table->tss_low;
    lo->limit_low   = (uint16_t)(limit & 0xFFFF);
    lo->base_low    = (uint16_t)(base & 0xFFFF);
    lo->base_mid    = (uint8_t)((base >> 16) & 0xFF);
    lo->access      = 0x89; /* present, DPL0, type=0x9 (64-bit TSS, available) */
    lo->granularity = (uint8_t)((limit >> 16) & 0x0F); /* G=0: byte granularity */
    lo->base_high   = (uint8_t)((base >> 24) & 0xFF);

    table->tss_high.base_upper32 = (uint32_t)(base >> 32);
    table->tss_high.reserved = 0;
}

void gdt_init(struct gdt_table *table, struct tss *tss, uint64_t ist1_stack_top) {
    memset(table, 0, sizeof(*table));
    memset(tss, 0, sizeof(*tss));

    set_entry(&table->kcode, 0x9A, 0xAF); /* present, ring0, code, R/X; L=1 long mode */
    set_entry(&table->kdata, 0x92, 0xCF); /* present, ring0, data, R/W */
    set_entry(&table->udata, 0xF2, 0xCF); /* present, ring3, data, R/W */
    set_entry(&table->ucode, 0xFA, 0xAF); /* present, ring3, code, R/X; L=1 */

    tss->ist1 = ist1_stack_top;
    tss->iomap_base = sizeof(struct tss); /* no I/O bitmap: all ports trap at CPL3 */
    set_tss_descriptor(table, tss);

    struct gdtr ptr = {
        .limit = sizeof(*table) - 1,
        .base = (uint64_t)table,
    };

    asm volatile ("lgdt %0" : : "m"(ptr));

    /* Reload every data segment register with the flat kernel data
     * selector, and CS with a far return trick (there is no direct
     * "mov cs, imm" on x86 -- CS can only change via a control-transfer
     * instruction). */
    asm volatile (
        "mov %0, %%ax\n\t"
        "mov %%ax, %%ds\n\t"
        "mov %%ax, %%es\n\t"
        "mov %%ax, %%fs\n\t"
        "mov %%ax, %%gs\n\t"
        "mov %%ax, %%ss\n\t"
        "lea 1f(%%rip), %%rax\n\t"
        "pushq %1\n\t"
        "pushq %%rax\n\t"
        "lretq\n\t"
        "1:\n\t"
        :
        : "i"(GDT_KERNEL_DATA), "i"((uint64_t)GDT_KERNEL_CODE)
        : "rax", "memory"
    );

    asm volatile ("ltr %0" : : "r"((uint16_t)GDT_TSS));
}

void tss_set_rsp0(struct tss *tss, uint64_t rsp0) {
    tss->rsp0 = rsp0;
}
