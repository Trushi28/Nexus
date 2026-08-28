/*
 * Fault-safe user<->kernel copy primitives.
 *
 * Each routine has one faultable instruction (`rep movsb`), allowing the
 * page-fault handler to redirect faults to a fixup. Returns 0 on success
 * or the number of bytes not copied.
 *
 * Arguments: rdi = dst, rsi = src, rdx = len.
 */
.section .text

.global __copy_from_user_asm
.global __copy_from_user_fault_ip
.global __copy_from_user_fixup

__copy_from_user_asm:
	mov %rdx, %rcx

__copy_from_user_fault_ip:
	rep movsb
	xor %eax, %eax
	ret

__copy_from_user_fixup:
	mov %rcx, %rax
	ret

.global __copy_to_user_asm
.global __copy_to_user_fault_ip
.global __copy_to_user_fixup

__copy_to_user_asm:
	mov %rdx, %rcx

__copy_to_user_fault_ip:
	rep movsb
	xor %eax, %eax
	ret

__copy_to_user_fixup:
	mov %rcx, %rax
	ret

.section .note.GNU-stack, "", @progbits
