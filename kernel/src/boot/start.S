/* _start is what Limine actually jumps to (see linker.ld's ENTRY(_start)
 * and the kernel address/paging guarantees the base revision 6 request
 * gives us). Its only job is to get off of whatever stack the bootloader
 * handed us and onto one we own outright, before any C code runs.
 *
 * Why this matters: kmain() eventually builds our own page tables and
 * switches CR3 to them (see mm/vmm.c). Our own tables are built to cover
 * the kernel image (.text/.rodata/.data/.bss, via the Executable Address
 * feature) and the full direct map -- but nothing guarantees the
 * bootloader's *original* entry stack lives at a virtual address that
 * happens to still be valid after that switch. A stack backed by our own
 * .bss, on the other hand, is guaranteed mapped both before and after
 * (Limine has to map the whole kernel image just to execute the first
 * instruction here at all, and vmm_init() maps the identical range in
 * our own tables from the same Executable Address response). So: use
 * .bss, not whatever RSP arrived with.
 */

.section .bss
.align 16
boot_stack_bottom:
.skip 65536 /* 64 KiB boot/BSP stack */
boot_stack_top:

.section .text
.global _start
.extern kmain
_start:
    lea boot_stack_top(%rip), %rsp
    xor %rbp, %rbp      /* terminate stack traces here */
    and $~0xF, %rsp      /* 16-byte align, satisfying the SysV ABI */
    call kmain
    /* kmain() is declared noreturn and never actually returns, but if
     * something has gone very wrong, don't run off into the weeds. */
.Lhang:
    cli
    hlt
    jmp .Lhang

.section .note.GNU-stack, "", @progbits
