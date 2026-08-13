.section .text
.global  context_switch

context_switch:
	push %rbx
	push %rbp
	push %r12
	push %r13
	push %r14
	push %r15

	mov  %rsp, (%rdi) // *old_rsp_out = rsp
	movb $1, (%rdx)
	mov  %rsi, %rsp   // rsp = new_rsp

	pop %r15
	pop %r14
	pop %r13
	pop %r12
	pop %rbp
	pop %rbx

	ret

.section .note.GNU-stack, "", @progbits
