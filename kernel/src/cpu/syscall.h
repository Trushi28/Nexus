#ifndef NEXUS_SYSCALL_H
#define NEXUS_SYSCALL_H

/* Registers the syscall dispatcher on VEC_SYSCALL. Call once, after
 * idt_init() (which is what actually gives that vector its DPL=3
 * gate -- see cpu/idt.c). */
void syscall_init(void);

#endif /* NEXUS_SYSCALL_H */
