/* void context_switch(uint64_t *old_rsp_out, uint64_t new_rsp);
 *
 * Deliberately minimal: only the SysV-ABI callee-saved registers
 * (rbx, rbp, r12-r15) and RSP itself need saving -- everything else is
 * already caller-saved by whatever C code called us, per the ABI. RFLAGS
 * is *not* part of the saved state on purpose: interrupt gates already
 * save/restore it via the per-task interrupt frame that sits on each
 * task's own stack for preemption, and schedule()'s explicit sti() at
 * its tail handles the voluntary-yield path. See sched/sched.c.
 *
 * For a brand new task, sched_task_create() pre-builds a fake stack
 * image that looks exactly like what the six pushes below would have
 * produced, with task_bootstrap as the "return address" -- so the `ret`
 * here transparently starts a new task the very first time it runs.
 */

.section .text
.global context_switch
context_switch:
    push %rbx
    push %rbp
    push %r12
    push %r13
    push %r14
    push %r15

    mov %rsp, (%rdi)   /* *old_rsp_out = rsp */
    mov %rsi, %rsp     /* rsp = new_rsp */

    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rbp
    pop %rbx

    ret

.section .note.GNU-stack, "", @progbits
