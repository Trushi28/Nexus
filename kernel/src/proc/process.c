#include "proc/process.h"
#include "abi/syscall_nr.h"
#include "debug/log.h"
#include "fs/vfs.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "proc/elf.h"
#include "sched/sched.h"

struct task *process_spawn(const char *path, const char *name) {
  struct vfs_file *file;
  if (!vfs_open(path, O_RDONLY, &file)) {
    kprintf("run: %s: no such file\n", path);
    return NULL;
  }

  uint64_t size = vfs_file_size(file);
  uint8_t *buf = kmalloc(size > 0 ? size : 1);
  if (buf == NULL) {
    kprintf("run: out of memory reading %s\n", path);
    vfs_close(file);
    return NULL;
  }
  size_t got = vfs_read(file, buf, size);
  vfs_close(file);
  if (got != size) {
    kprintf("run: short read on %s\n", path);
    kfree(buf);
    return NULL;
  }

  uint64_t pml4 = vmm_new_address_space();
  if (pml4 == 0) {
    kprintf("run: out of memory creating an address space for %s\n", path);
    kfree(buf);
    return NULL;
  }

  struct elf_load_result elf;
  bool loaded = elf_load(pml4, buf, size, &elf);
  kfree(buf);
  if (!loaded) {
    kprintf("run: %s: not a runnable Nexus binary\n", path);
    vmm_free_user_space(pml4);
    return NULL;
  }

  uint64_t stack_top = USER_STACK_TOP;
  for (uint64_t i = 0; i < USER_STACK_PAGES; i++) {
    uint64_t phys = pmm_alloc_page();
    if (phys == 0) {
      kprintf("run: out of memory mapping the user stack for %s\n", path);
      vmm_free_user_space(pml4);
      return NULL;
    }
    uint64_t va = stack_top - (i + 1) * PAGE_SIZE;
    vmm_map_page_in(pml4, va, phys, VMM_WRITABLE | VMM_USER | VMM_NX);
  }

  struct task *t = task_create_user(name, pml4, elf.entry, stack_top, 0, 0);
  if (t == NULL) {
    vmm_free_user_space(pml4);
    return NULL;
  }

  t->brk_start = ALIGN_UP(elf.highest_vaddr, PAGE_SIZE);
  t->brk = t->brk_start;
  task_publish(t); /* only now, with brk_start/brk already set -- see
                       task_publish()'s comment in sched.h */
  return t;
}

int process_wait(struct task *child) { return sched_wait_task(child); }
