/* Generates isr_stub_0 .. isr_stub_255 plus isr_stub_table[256], and the
 * common trampoline every one of them funnels into.
 *
 * x86-64 fact this file leans on: in long mode IRETQ *always* pops all
 * five stack-frame words (RIP, CS, RFLAGS, RSP, SS), regardless of
 * whether a privilege change or IST stack switch occurred -- unlike
 * legacy 32-bit mode, where SS:ESP are conditional. Symmetrically, the
 * CPU always pushes all five on entry too. That means one uniform frame
 * layout works for every vector, with no special-casing.
 * (AMD64 Architecture Programmer's Manual Vol. 2, sec. "Interrupt Stack
 * Frame"; Intel SDM Vol. 3A likewise.)
 */

.altmacro

.macro isr_stub_noerr num
.global isr_stub_\num
isr_stub_\num:
    push $0
    push $\num
    jmp isr_common_stub
.endm

.macro isr_stub_err num
.global isr_stub_\num
isr_stub_\num:
    push $\num
    jmp isr_common_stub
.endm

.section .text

i = 0
.rept 256
.if i==8
    isr_stub_err %i
.elseif i==10
    isr_stub_err %i
.elseif i==11
    isr_stub_err %i
.elseif i==12
    isr_stub_err %i
.elseif i==13
    isr_stub_err %i
.elseif i==14
    isr_stub_err %i
.elseif i==17
    isr_stub_err %i
.elseif i==21
    isr_stub_err %i
.elseif i==29
    isr_stub_err %i
.elseif i==30
    isr_stub_err %i
.else
    isr_stub_noerr %i
.endif
i = i + 1
.endr

.extern isr_common_handler

isr_common_stub:
    push %rax
    push %rbx
    push %rcx
    push %rdx
    push %rsi
    push %rdi
    push %rbp
    push %r8
    push %r9
    push %r10
    push %r11
    push %r12
    push %r13
    push %r14
    push %r15

    mov %rsp, %rdi   /* struct interrupt_frame *frame -- first SysV arg */
    cld              /* ABI requires DF clear on entry to any C function */
    call isr_common_handler

    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %r11
    pop %r10
    pop %r9
    pop %r8
    pop %rbp
    pop %rdi
    pop %rsi
    pop %rdx
    pop %rcx
    pop %rbx
    pop %rax

    add $16, %rsp    /* drop our pushed vector + error_code */
    iretq

.macro table_entry num
.quad isr_stub_\num
.endm

.section .data
.global isr_stub_table
isr_stub_table:
i = 0
.rept 256
    table_entry %i
    i = i + 1
.endr

.section .note.GNU-stack, "", @progbits
