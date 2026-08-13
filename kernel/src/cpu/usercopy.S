/* Raw user<->kernel copy primitives. Written in asm, not C, so there is
 * exactly one instruction per routine that can actually fault on a bad
 * user address (a `rep movsb`), letting cpu/isr.c's page-fault handler
 * recognise that one instruction by address and divert to a fixup
 * instead of the usual dump_and_panic() path -- see cpu/usercopy.c and
 * the exception table in cpu/isr.c.
 *
 * Calling convention (both routines): rdi = dst, rsi = src, rdx = len.
 * Returns 0 on success, or however many bytes were NOT copied if a
 * fault hit partway through -- the CPU updates %rcx to "iterations
 * remaining" before it reports a fault on a rep-prefixed instruction,
 * so the fixup just hands that straight back.
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
