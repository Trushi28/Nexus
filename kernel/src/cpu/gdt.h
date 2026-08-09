#ifndef NEXUS_GDT_H
#define NEXUS_GDT_H

#include "klib/klib.h"

#define GDT_KERNEL_CODE 0x08
#define GDT_KERNEL_DATA 0x10
#define GDT_USER_DATA   (0x18 | 3)
#define GDT_USER_CODE   (0x20 | 3)
#define GDT_TSS         0x28

struct PACKED gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
};

struct PACKED tss_entry_high {
    uint32_t base_upper32;
    uint32_t reserved;
};

struct PACKED tss {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist1;
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
};

struct PACKED gdt_table {
    struct gdt_entry null;
    struct gdt_entry kcode;
    struct gdt_entry kdata;
    struct gdt_entry udata;
    struct gdt_entry ucode;
    struct gdt_entry tss_low;
    struct tss_entry_high tss_high;
};

struct PACKED gdtr {
    uint16_t limit;
    uint64_t base;
};

/* Sets up and loads a fresh GDT + TSS for the calling CPU. `ist1_stack_top`
 * is used as the #DF (double fault) and NMI emergency stack -- a separate
 * known-good stack that interrupt delivery switches to unconditionally,
 * so a fault caused by stack corruption doesn't also corrupt the fault
 * handler's own frame. */
void gdt_init(struct gdt_table *table, struct tss *tss, uint64_t ist1_stack_top);

/* Updates RSP0 (the stack the CPU switches to when an interrupt/exception
 * hits us while previously running at a lower privilege level). Kept even
 * though Nexus doesn't have ring-3 tasks yet -- the scheduler updates
 * it on every context switch so it is correct the moment usermode lands. */
void tss_set_rsp0(struct tss *tss, uint64_t rsp0);

#endif /* NEXUS_GDT_H */
