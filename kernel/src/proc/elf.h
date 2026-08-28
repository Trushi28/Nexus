#ifndef NEXUS_ELF_H
#define NEXUS_ELF_H

#include "klib/klib.h"

/* Minimal ELF64 definitions for static, non-PIE ET_EXEC binaries. */

typedef struct PACKED {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} elf64_ehdr_t;

typedef struct PACKED {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} elf64_phdr_t;

#define PT_LOAD 1

#define PF_X 1
#define PF_W 2
#define PF_R 4

#define ET_EXEC   2
#define EM_X86_64 62

struct elf_load_result {
    uint64_t entry;
    uint64_t highest_vaddr; // Page-aligned end of the loaded image.
};

/*
 * Loads all PT_LOAD segments into `pml4_phys`.
 * Returns true on success and fills `out`; on failure, logs the error
 * and returns false. The caller must destroy the address space on failure.
 */
bool elf_load(uint64_t pml4_phys, const uint8_t *data, size_t size, struct elf_load_result *out);

#endif /* NEXUS_ELF_H */
