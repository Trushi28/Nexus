#include "proc/elf.h"
#include "boot/requests.h"
#include "debug/log.h"
#include "mm/pmm.h"
#include "mm/vmm.h"

bool elf_load(uint64_t pml4_phys, const uint8_t *data, size_t size,
              struct elf_load_result *out) {
  if (size < sizeof(elf64_ehdr_t)) {
    kprintf("[elf] file too small to hold an ELF header\n");
    return false;
  }

  const elf64_ehdr_t *eh = (const elf64_ehdr_t *)data;
  if (eh->e_ident[0] != 0x7f || eh->e_ident[1] != 'E' ||
      eh->e_ident[2] != 'L' || eh->e_ident[3] != 'F') {
    kprintf("[elf] bad magic\n");
    return false;
  }
  if (eh->e_ident[4] != 2 /* ELFCLASS64 */ ||
      eh->e_ident[5] != 1 /* ELFDATA2LSB */) {
    kprintf("[elf] not a little-endian 64-bit ELF\n");
    return false;
  }
  if (eh->e_machine != EM_X86_64 || eh->e_type != ET_EXEC) {
    kprintf("[elf] not a static x86-64 executable (type=%u machine=%u)\n",
            eh->e_type, eh->e_machine);
    return false;
  }
  if (eh->e_phentsize != sizeof(elf64_phdr_t)) {
    /* The bounds check just below trusts e_phentsize to match the
     * stride we actually index phdrs[] with (sizeof(elf64_phdr_t),
     * the compiler's stride) -- if a file lies and declares a
     * smaller one, that check can pass while phdrs[i] still reads
     * past `size`. Refuse instead of trusting attacker-controlled
     * (or just corrupt) input here. */
    kprintf("[elf] unexpected e_phentsize %u (expected %lu)\n", eh->e_phentsize,
            (uint64_t)sizeof(elf64_phdr_t));
    return false;
  }
  if (eh->e_phoff == 0 || eh->e_phnum == 0 ||
      (uint64_t)eh->e_phoff + (uint64_t)eh->e_phnum * eh->e_phentsize > size) {
    kprintf("[elf] bad or truncated program header table\n");
    return false;
  }

  uint64_t highest = 0;
  bool entry_is_mapped = false;
  const elf64_phdr_t *phdrs = (const elf64_phdr_t *)(data + eh->e_phoff);

  for (uint16_t i = 0; i < eh->e_phnum; i++) {
    const elf64_phdr_t *ph = &phdrs[i];
    if (ph->p_type != PT_LOAD) {
      continue;
    }
    if (ph->p_memsz == 0) {
      /* A vacuous segment -- GNU ld emits one of these at vaddr 0
       * for a PHDR class with no input sections (e.g. :data when
       * a program has no writable globals at all, as with every
       * userland/ demo here). Nothing to map, nothing to check. */
      continue;
    }
    if (ph->p_filesz > ph->p_memsz) {
      kprintf("[elf] segment %u: filesz > memsz\n", i);
      return false;
    }
    if ((uint64_t)ph->p_offset + ph->p_filesz > size) {
      kprintf("[elf] segment %u reaches past the end of the file\n", i);
      return false;
    }
    if (ph->p_vaddr < PAGE_SIZE || ph->p_vaddr >= 0x0000800000000000ULL ||
        ph->p_vaddr + ph->p_memsz < ph->p_vaddr ||
        ph->p_vaddr + ph->p_memsz >= 0x0000800000000000ULL) {
      /* Refuse the null-guard page and anything reaching into
       * canonical higher-half/kernel territory -- a malformed
       * (or actively hostile) ELF could otherwise ask us to map
       * over the shared kernel portion of its own address space. */
      kprintf("[elf] segment %u vaddr 0x%p out of the allowed user range\n", i,
              (void *)ph->p_vaddr);
      return false;
    }
    if (ph->p_vaddr <= eh->e_entry && eh->e_entry < ph->p_vaddr + ph->p_memsz) {
      entry_is_mapped = true;
    }

    uint64_t seg_start = ALIGN_DOWN(ph->p_vaddr, PAGE_SIZE);
    uint64_t seg_end = ALIGN_UP(ph->p_vaddr + ph->p_memsz, PAGE_SIZE);

    uint64_t flags = VMM_USER;
    if (ph->p_flags & PF_W) {
      flags |= VMM_WRITABLE;
    }
    if (!(ph->p_flags & PF_X)) {
      flags |= VMM_NX;
    }

    for (uint64_t va = seg_start; va < seg_end; va += PAGE_SIZE) {
      uint64_t phys =
          pmm_alloc_page(); /* zeroed -- covers the BSS tail for free */
      if (phys == 0) {
        kprintf("[elf] out of memory mapping segment %u\n", i);
        return false;
      }
      vmm_map_page_in(pml4_phys, va, phys, flags);

      /* Copy whatever slice of the file's segment data lands in
       * *this* page. Doing the copy right here (rather than in a
       * second pass) means we never need to translate a user
       * virtual address back to a physical one -- we already
       * have `phys` fresh out of pmm_alloc_page(). */
      uint64_t page_file_start = MAX(va, ph->p_vaddr);
      uint64_t seg_file_end = ph->p_vaddr + ph->p_filesz;
      uint64_t page_end = va + PAGE_SIZE;
      uint64_t copy_end = MIN(page_end, seg_file_end);

      if (copy_end > page_file_start) {
        uint64_t len = copy_end - page_file_start;
        uint64_t file_off = ph->p_offset + (page_file_start - ph->p_vaddr);
        uint64_t dst_off = page_file_start - va;
        memcpy((uint8_t *)phys_to_virt(phys) + dst_off, data + file_off, len);
      }
      /* Anything past filesz within memsz is BSS and is already
       * zero, courtesy of pmm_alloc_page(). */
    }

    if (seg_end > highest) {
      highest = seg_end;
    }
  }

  if (highest == 0) {
    kprintf("[elf] no PT_LOAD segments -- nothing to run\n");
    return false;
  }
  if (!entry_is_mapped) {
    kprintf("[elf] entry point 0x%p isn't inside any loaded segment\n",
            (void *)eh->e_entry);
    return false;
  }

  out->entry = eh->e_entry;
  out->highest_vaddr = highest;
  return true;
}
