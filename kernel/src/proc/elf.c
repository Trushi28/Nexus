#include "proc/elf.h"
#include "boot/requests.h"
#include "debug/log.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "proc/process.h"

/* Hard, enforced ceilings against a malformed or actively hostile ELF
 * -- see validate_segments()'s own comment for what these catch that
 * the original per-segment checks couldn't. Neither is reachable by
 * anything this build actually produces (userland/user.ld emits at
 * most 3 PT_LOAD segments, comfortably under either number); they
 * exist purely as a backstop against whatever gets fed to `run`/`exec`
 * next. */
#define ELF_MAX_PHDRS 64
#define ELF_MAX_TOTAL_PAGES                                                    \
  (256ull * 1024) /* 1 GiB across every PT_LOAD segment combined */

static bool ranges_overlap(uint64_t a_start, uint64_t a_end, uint64_t b_start,
                           uint64_t b_end) {
  return a_start < b_end && b_start < a_end;
}

/* Pre-pass over every PT_LOAD header, run AFTER the per-segment bounds
 * checks in elf_load() below (so every p_vaddr/p_memsz here is already
 * individually sane) and BEFORE anything gets mapped. Catches what a
 * single segment can't see on its own:
 *
 *   - Two segments whose page-aligned ranges overlap each other.
 *     elf_load() maps pages by walking segments in order and calling
 *     vmm_map_page_in() per page with no "is this already mapped"
 *     check -- that function just overwrites whatever PTE was there,
 *     silently leaking the physical frame the first mapping pointed
 *     at and leaving that page's permissions determined by whichever
 *     segment happened to be processed last.
 *   - A segment landing on the fixed virtual address
 *     process_spawn()/sys_exec_impl() always map the user stack at
 *     (USER_STACK_TOP downward, USER_STACK_PAGES deep -- see
 *     proc/process.h). elf_load() itself never touches that range,
 *     but the CALLER maps the stack there unconditionally right after
 *     a successful load -- a segment claiming that address would load
 *     clean and then have its own data silently overwritten by zeroed
 *     stack pages the instant the process's stack gets set up. A
 *     binary that loads fine and corrupts itself on first run is a
 *     far worse failure than refusing to load it at all.
 *   - A combined PT_LOAD footprint large enough to be a resource-
 *     exhaustion concern on its own, independent of whether physical
 *     memory eventually runs out gracefully (it does -- pmm_alloc_page()
 *     returning 0 is already handled -- but a multi-hundred-GiB claim
 *     from a kilobyte-sized file has no legitimate reason to even try).
 *
 * Runs entirely against the program header table with nothing mapped
 * yet, so a rejection here costs nothing to unwind. */
static bool validate_segments(const elf64_phdr_t *phdrs, uint16_t phnum) {
  uint64_t stack_region_end = USER_STACK_TOP;
  uint64_t stack_region_start =
      USER_STACK_TOP - (uint64_t)USER_STACK_PAGES * PAGE_SIZE;

  struct {
    uint64_t start, end;
  } loads[ELF_MAX_PHDRS];
  uint32_t load_count = 0;
  uint64_t total_pages = 0;

  for (uint16_t i = 0; i < phnum; i++) {
    const elf64_phdr_t *ph = &phdrs[i];
    if (ph->p_type != PT_LOAD || ph->p_memsz == 0) {
      continue;
    }

    uint64_t seg_start = ALIGN_DOWN(ph->p_vaddr, PAGE_SIZE);
    uint64_t seg_end = ALIGN_UP(ph->p_vaddr + ph->p_memsz, PAGE_SIZE);

    if (ranges_overlap(seg_start, seg_end, stack_region_start,
                       stack_region_end)) {
      kprintf("[elf] segment %u (0x%p-0x%p) overlaps the fixed user stack "
              "region (0x%p-0x%p) -- refusing to load\n",
              i, (void *)seg_start, (void *)seg_end, (void *)stack_region_start,
              (void *)stack_region_end);
      return false;
    }

    for (uint32_t j = 0; j < load_count; j++) {
      if (ranges_overlap(seg_start, seg_end, loads[j].start, loads[j].end)) {
        kprintf("[elf] segment %u (0x%p-0x%p) overlaps an earlier PT_LOAD "
                "segment (0x%p-0x%p) -- refusing to load\n",
                i, (void *)seg_start, (void *)seg_end, (void *)loads[j].start,
                (void *)loads[j].end);
        return false;
      }
    }

    if (load_count >= ELF_MAX_PHDRS) {
      /* Unreachable in practice -- e_phnum itself is already capped
       * at ELF_MAX_PHDRS by elf_load() before this function is ever
       * called. Checked anyway rather than trusted, same defensive
       * posture as the rest of this pass. */
      kprintf("[elf] more than %u PT_LOAD segments -- refusing to load\n",
              ELF_MAX_PHDRS);
      return false;
    }
    loads[load_count].start = seg_start;
    loads[load_count].end = seg_end;
    load_count++;

    total_pages += (seg_end - seg_start) / PAGE_SIZE;
    if (total_pages > ELF_MAX_TOTAL_PAGES) {
      kprintf("[elf] PT_LOAD segments claim more than %lu MiB combined -- "
              "refusing to load\n",
              (uint64_t)(ELF_MAX_TOTAL_PAGES * PAGE_SIZE) / (1024 * 1024));
      return false;
    }
  }

  return true;
}

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
  if (eh->e_phnum > ELF_MAX_PHDRS) {
    kprintf("[elf] %u program headers exceeds this loader's %u-entry cap\n",
            eh->e_phnum, ELF_MAX_PHDRS);
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

    uint64_t seg_end = ALIGN_UP(ph->p_vaddr + ph->p_memsz, PAGE_SIZE);
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

  /* Cross-segment/stack-overlap validation -- see validate_segments()'s
   * own comment. Runs after every per-segment check above has already
   * confirmed each individual p_vaddr/p_memsz is sane, and before
   * anything below maps a single page. */
  if (!validate_segments(phdrs, eh->e_phnum)) {
    return false; /* already logged why */
  }

  for (uint16_t i = 0; i < eh->e_phnum; i++) {
    const elf64_phdr_t *ph = &phdrs[i];
    if (ph->p_type != PT_LOAD || ph->p_memsz == 0) {
      continue;
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
  }

  out->entry = eh->e_entry;
  out->highest_vaddr = highest;
  return true;
}
