/* Nexus kernel linker script.
 *
 * Based on the standard Limine higher-half bare-bones layout, extended
 * with explicit, page-aligned symbols marking the start/end of each
 * permission class (text/rodata/data+bss). The VMM uses these to build
 * page tables that are actually W^X: .text is R+X, .rodata is R only,
 * .data/.bss are R+W, all NX except .text.
 */

OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

PHDRS
{
    text    PT_LOAD    FLAGS((1 << 0) | (1 << 2)) ; /* Execute + Read */
    rodata  PT_LOAD    FLAGS((1 << 2)) ;             /* Read only */
    data    PT_LOAD    FLAGS((1 << 1) | (1 << 2)) ;  /* Write + Read */
    dynamic PT_DYNAMIC FLAGS((1 << 1) | (1 << 2)) ;  /* Relocations (PIE) */
}

SECTIONS
{
    /* Nexus is loaded in the topmost 2GiB of virtual address space, as
     * mandated by the Limine boot protocol for non-relocatable placement
     * expectations; being a relocatable PIE, the bootloader may slide us,
     * but never below this base. */
    . = 0xffffffff80000000;

    __kernel_start = .;

    .text : {
        __text_start = .;
        *(.text .text.*)
        __text_end = .;
    } :text

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    .rodata : {
        __rodata_start = .;
        *(.rodata .rodata.*)
        __rodata_end = .;
    } :rodata

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    .data : {
        __data_start = .;
        *(.data .data.*)

        /* Limine's request-scanning protocol requires these three
         * pieces to appear in THIS order in memory: it walks forward
         * from the start marker and stops at the end marker, ignoring
         * anything outside that range. Without an explicit rule here,
         * the linker treats .requests/.requests_start_marker/
         * .requests_end_marker as orphan sections and is free to place
         * them in any order (alphabetical, in our case) -- which broke
         * every single boot request, not just the base revision one. */
        KEEP(*(.requests_start_marker))
        KEEP(*(.requests))
        KEEP(*(.requests_end_marker))
    } :data

    /* Dynamic section for relocations, both in its own PHDR and inside
     * the data PHDR (required for the kernel to be a valid PIE). */
    .dynamic : {
        *(.dynamic)
    } :data :dynamic

    /* NOTE: .bss must be the last thing mapped to :data, otherwise a lot
     * of needless zeros get written into the kernel image on disk. */
    .bss : {
        *(COMMON)
        *(.bss .bss.*)
        __bss_end = .;
    } :data

    __data_end = __bss_end;
    __kernel_end = .;

    /* Discard sections that either cause problems on some hosts or that
     * we simply have no use for in a freestanding kernel. */
    /DISCARD/ : {
        *(.eh_frame*)
        *(.note .note.*)
        *(.comment)
    }
}
