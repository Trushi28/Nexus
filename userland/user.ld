/* Nexus userland linker script.
 *
 * Produces a static, non-relocatable ET_EXEC binary -- exactly what
 * kernel/src/proc/elf.c's loader supports (no PIE, no dynamic linking,
 * no relocations). Mirrors the kernel's own linker.ld in spirit: W^X,
 * one PT_LOAD segment per permission class, each page-aligned so the
 * loader never has to split a single page across two different
 * permission sets.
 */

OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

PHDRS
{
    text   PT_LOAD FLAGS((1 << 0) | (1 << 2)) ; /* Execute + Read */
    rodata PT_LOAD FLAGS((1 << 2)) ;             /* Read only */
    data   PT_LOAD FLAGS((1 << 1) | (1 << 2)) ;  /* Write + Read */
}

SECTIONS
{
    /* Low, fixed, well clear of the null-guard page elf_load() refuses
     * -- the classic default base for a small static x86-64 ET_EXEC. */
    . = 0x400000;

    .text : {
        *(.text .text.*)
    } :text

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    .rodata : {
        *(.rodata .rodata.*)
    } :rodata

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    .data : {
        *(.data .data.*)
    } :data

    /* .bss last within :data, same reasoning as the kernel's own
     * linker.ld: keeps zeros out of the file instead of the image. */
    .bss : {
        *(COMMON)
        *(.bss .bss.*)
    } :data

    /DISCARD/ : {
        *(.eh_frame*)
        *(.note .note.*)
        *(.comment)
    }
}
