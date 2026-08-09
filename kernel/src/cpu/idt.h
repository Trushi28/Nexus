#ifndef NEXUS_IDT_H
#define NEXUS_IDT_H

#include "klib/klib.h"

/* Builds the (single, shared-across-all-CPUs) IDT and loads it on the
 * calling CPU. Call once on the BSP; every AP just needs idt_load(). */
void idt_init(void);
void idt_load(void);

#endif /* NEXUS_IDT_H */
