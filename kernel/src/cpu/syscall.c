#include "cpu/syscall.h"
#include "abi/syscall_nr.h"
#include "abi/task_info.h"
#include "cpu/cpu.h"
#include "cpu/io.h"
#include "cpu/isr.h"
#include "cpu/usercopy.h"
#include "cpu/vectors.h"
#include "debug/log.h"
#include "debug/serial.h"
#include "drivers/keyboard.h"
#include "fs/vfs.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "proc/elf.h"
#include "proc/process.h"
#include "sched/sched.h"
#include "time/timer.h"
#include "video/console.h"
/*
 * The syscall ABI: `int $0x80`, number in rax, args in rdi/rsi/rdx/r10/
 * r8, return value in rax. isr_stubs.S has already pushed every GPR
 * into `frame` by the time syscall_dispatch() runs (see cpu/isr.c), so
 * this is just an ordinary registered VEC_SYSCALL handler -- no new
 * assembly needed anywhere, only the DPL=3 IDT gate set up in
 * cpu/idt.c.
 *
 * Every syscall that touches a user-supplied pointer goes through
 * user_range_ok() first (rejects NULL-page and kernel-half pointers
 * outright) and then copy_from_user()/copy_to_user()/
 * copy_string_from_user() (cpu/usercopy.h) for the actual access. Those
 * survive a syntactically-valid-but-unmapped pointer: the one risky
 * instruction inside each is wrapped in a small exception table
 * cpu/isr.c's page-fault path consults, so a bad pointer now fails the
 * syscall instead of taking the whole kernel down with an unhandled
 * #PF. See cpu/usercopy.S for exactly how.*/
static bool user_range_ok(uint64_t addr, uint64_t len) {
  if (len == 0) {
    return true;
  }
  if (addr < PAGE_SIZE) {
    return false; /* NULL-page guard */
  }
  if (addr + len < addr) {
    return false; /* overflow */
  }
  if (addr + len > 0x0000800000000000ULL) {
    return false; /* canonical low half only -- never let a syscall
                      arg point into kernel/direct-map territory */
  }
  return true;
}

static void sys_exit_impl(struct interrupt_frame *f) {
  struct task *t = this_cpu()->current_task;
  t->exit_code = (int)f->rdi;
  task_exit(); /* never returns */
}

static bool fd_readable(struct vfs_file *file) {
  int mode = file->flags & O_ACCMODE;
  return mode == O_RDONLY || mode == O_RDWR;
}

static bool fd_writable(struct vfs_file *file) {
  int mode = file->flags & O_ACCMODE;
  return mode == O_WRONLY || mode == O_RDWR;
}

static void sys_write_impl(struct interrupt_frame *f) {
  int fd = (int)f->rdi;
  uint64_t buf = f->rsi;
  uint64_t len = f->rdx;

  if (!user_range_ok(buf, len)) {
    f->rax = (uint64_t)-1;
    return;
  }

  if (fd == STDOUT_FILENO || fd == STDERR_FILENO) {
    char chunk[128];
    uint64_t done = 0;
    while (done < len) {
      uint64_t n = MIN(len - done, sizeof(chunk) - 1);
      if (copy_from_user(chunk, (const void *)(buf + done), n) != 0) {
        f->rax = done > 0 ? done : (uint64_t)-1;
        return;
      }
      chunk[n] = '\0';
      kprintf("%s", chunk);
      done += n;
    }
    f->rax = len;
    return;
  }

  struct task *t = this_cpu()->current_task;
  if (fd < 0 || fd >= PROC_MAX_FDS || t->fds[fd] == NULL) {
    f->rax = (uint64_t)-1;
    return;
  }
  if (!fd_writable(t->fds[fd])) {
    f->rax = (uint64_t)-1; /* opened O_RDONLY -- not ours to write */
    return;
  }

  char chunk[128];
  uint64_t done = 0;
  while (done < len) {
    uint64_t n = MIN(len - done, sizeof(chunk));
    if (copy_from_user(chunk, (const void *)(buf + done), n) != 0) {
      break;
    }
    size_t put = vfs_write(t->fds[fd], chunk, n);
    done += put;
    if (put != n) {
      break; /* short write -- e.g. OOM growing the file */
    }
  }
  f->rax = done;
}

static void sys_read_impl(struct interrupt_frame *f) {
  int fd = (int)f->rdi;
  uint64_t buf = f->rsi;
  uint64_t len = f->rdx;

  if (!user_range_ok(buf, len)) {
    f->rax = (uint64_t)-1;
    return;
  }

  if (fd == STDIN_FILENO) {
    uint64_t n = 0;
    while (n < len) {
      char c = keyboard_getc(); /* blocks -- fine, we're not holding anything */
      if (c == KEY_UP || c == KEY_DOWN) {
        continue;
      }
      if (c == '\b') {
        if (n > 0) {
          n--;
          serial_puts("\b \b");
          console_backspace();
        }
        continue;
      }

      if (copy_to_user((void *)(buf + n), &c, 1) != 0) {
        break;
      }
      n++;

      if (c == '\n') {
        kprintf("\n");
        break;
      }
      if (c >= 0x20 && c < 0x7F) {
        kprintf("%c", c);
      }
    }
    f->rax = n;
    return;
  }

  struct task *t = this_cpu()->current_task;
  if (fd < 0 || fd >= PROC_MAX_FDS || t->fds[fd] == NULL) {
    f->rax = (uint64_t)-1;
    return;
  }
  if (!fd_readable(t->fds[fd])) {
    f->rax = (uint64_t)-1; /* opened O_WRONLY -- not ours to read */
    return;
  }

  char chunk[128];
  uint64_t done = 0;
  while (done < len) {
    uint64_t n = MIN(len - done, sizeof(chunk));
    size_t got = vfs_read(t->fds[fd], chunk, n);
    if (got == 0) {
      break;
    }
    if (copy_to_user((void *)(buf + done), chunk, got) != 0) {
      break;
    }
    done += got;
    if (got < n) {
      break; /* short read -- EOF */
    }
  }
  f->rax = done;
}

static void sys_open_impl(struct interrupt_frame *f) {
  uint64_t path_va = f->rdi;
  int flags = (int)f->rsi;

  char path[256];
  if (!user_range_ok(path_va, 1) ||
      !copy_string_from_user(path, (const void *)path_va, sizeof(path))) {
    f->rax = (uint64_t)-1;
    return;
  }

  struct task *t = this_cpu()->current_task;
  int slot = -1;
  for (int i = 0; i < PROC_MAX_FDS; i++) {
    if (t->fds[i] == NULL) {
      slot = i;
      break;
    }
  }
  if (slot < 0) {
    f->rax = (uint64_t)-1; /* fd table full */
    return;
  }

  struct vfs_file *file;
  if (!vfs_open_as(path, flags, t->uid, &file)) {
    f->rax = (uint64_t)-1;
    return;
  }

  if ((flags & O_TRUNC) != 0 && !vfs_truncate(file)) {
    vfs_close(file);
    f->rax = (uint64_t)-1;
    return;
  }

  t->fds[slot] = file;
  f->rax = (uint64_t)slot;
}

static void sys_close_impl(struct interrupt_frame *f) {
  int fd = (int)f->rdi;
  struct task *t = this_cpu()->current_task;
  if (fd < 0 || fd >= PROC_MAX_FDS || t->fds[fd] == NULL) {
    f->rax = (uint64_t)-1;
    return;
  }
  vfs_close(t->fds[fd]);
  t->fds[fd] = NULL;
  f->rax = 0;
}

static void sys_getpid_impl(struct interrupt_frame *f) {
  f->rax = this_cpu()->current_task->id;
}

static void sys_getuid_impl(struct interrupt_frame *f) {
  f->rax = this_cpu()->current_task->uid;
}

/* Root-only, one-way privilege drop -- see abi/syscall_nr.h's
 * SYS_setuid comment for why this is deliberately simpler than a real
 * setuid()/seteuid()/setresuid() triad: Nexus has no user database,
 * no login, no concept of "which uids are allowed to become which
 * other uids" beyond "uid 0 can become anything, nothing else can
 * become anything at all." A process that drops its own privilege
 * this way can never get it back -- there's no saved-uid to restore
 * from -- which is exactly the point: the intended use is a
 * root-launched process voluntarily narrowing itself (see nsh's
 * `drop` command) right before doing something it doesn't want to
 * fully trust itself with, not a general identity-switching
 * mechanism. */
static void sys_setuid_impl(struct interrupt_frame *f) {
  struct task *caller = this_cpu()->current_task;
  uint32_t requested = (uint32_t)f->rdi;

  if (caller->uid != 0) {
    f->rax = (uint64_t)-1;
    return;
  }
  caller->uid = requested;
  f->rax = 0;
}

static void sys_sleep_ms_impl(struct interrupt_frame *f) {
  sched_sleep_ms((uint32_t)f->rdi);
  f->rax = 0;
}

static void sys_yield_impl(struct interrupt_frame *f) {
  sched_yield();
  f->rax = 0;
}

static void sys_uptime_ms_impl(struct interrupt_frame *f) {
  f->rax = timer_uptime_ms();
}

static void sys_brk_impl(struct interrupt_frame *f) {
  struct task *t = this_cpu()->current_task;
  uint64_t req = f->rdi;

  if (req == 0) {
    f->rax = t->brk;
    return;
  }
  if (req < t->brk_start) {
    f->rax = t->brk; /* refuse to shrink below the loaded image */
    return;
  }
  uint64_t max_brk = t->brk_start + (uint64_t)USER_BRK_MAX_PAGES * PAGE_SIZE;
  if (req > max_brk) {
    f->rax = t->brk; /* refuse: v1 caps the user heap at USER_BRK_MAX_PAGES */
    return;
  }

  uint64_t old_top = ALIGN_UP(t->brk, PAGE_SIZE);
  uint64_t new_top = ALIGN_UP(req, PAGE_SIZE);

  if (new_top > old_top) {
    for (uint64_t va = old_top; va < new_top; va += PAGE_SIZE) {
      uint64_t phys = pmm_alloc_page();
      if (phys == 0) {
        f->rax = t->brk;
        return;
      }
      vmm_map_page_in(t->cr3_phys, va, phys,
                      VMM_PRESENT | VMM_WRITABLE | VMM_USER | VMM_NX);
      t->brk = va + PAGE_SIZE;
    }
  } else if (new_top < old_top) {
    vmm_unmap_range_in(t->cr3_phys, new_top, old_top - new_top);
  }

  t->brk = req;
  f->rax = t->brk;
}

static void sys_readdir_impl(struct interrupt_frame *f) {
  uint64_t path_va = f->rdi;
  uint32_t index = (uint32_t)f->rsi;
  uint64_t out_va = f->rdx;
  uint64_t out_len = f->r10;

  char path[256];
  if (!user_range_ok(path_va, 1) || !user_range_ok(out_va, out_len) ||
      !copy_string_from_user(path, (const void *)path_va, sizeof(path))) {
    f->rax = (uint64_t)-1;
    return;
  }

  char name[64];
  if (!vfs_readdir(path, index, name, sizeof(name))) {
    f->rax = (uint64_t)-1;
    return;
  }

  size_t n = strlen(name);
  if (out_len > 0 && n >= out_len) {
    n = out_len - 1;
  }
  if (out_len > 0) {
    if (copy_to_user((void *)out_va, name, n) != 0 ||
        copy_to_user((void *)(out_va + n), "", 1) != 0) {
      f->rax = (uint64_t)-1;
      return;
    }
  }
  f->rax = 0;
}

static void sys_spawn_impl(struct interrupt_frame *f) {
  uint64_t path_va = f->rdi;

  char path[256];
  if (!user_range_ok(path_va, 1) ||
      !copy_string_from_user(path, (const void *)path_va, sizeof(path))) {
    f->rax = (uint64_t)-1;
    return;
  }

  /* Same default an ordinary fork/exec makes: a spawned child
   * inherits the SPAWNING task's own clearance, not root's --
   * credentials only ever change via an explicit sys_setuid_impl()
   * call, or the kernel shell's own `runas`, which calls
   * process_spawn() directly with an explicit uid instead of going
   * through this syscall at all (see shell/shell.c's cmd_runas()). */
  struct task *caller = this_cpu()->current_task;
  struct task *t = process_spawn(path, path, caller->uid);
  f->rax = (t != NULL) ? t->id : (uint64_t)-1;
}

static void sys_exec_impl(struct interrupt_frame *f) {
  uint64_t path_va = f->rdi;

  char path[256];
  if (!user_range_ok(path_va, 1) ||
      !copy_string_from_user(path, (const void *)path_va, sizeof(path))) {
    f->rax = (uint64_t)-1;
    return;
  }

  struct vfs_file *file;
  if (!vfs_open(path, O_RDONLY, &file)) {
    f->rax = (uint64_t)-1;
    return;
  }
  uint64_t size = vfs_file_size(file);
  uint8_t *filebuf = kmalloc(size > 0 ? size : 1);
  if (filebuf == NULL) {
    vfs_close(file);
    f->rax = (uint64_t)-1;
    return;
  }
  size_t got = vfs_read(file, filebuf, size);
  vfs_close(file);
  if (got != size) {
    kfree(filebuf);
    f->rax = (uint64_t)-1;
    return;
  }

  uint64_t new_pml4 = vmm_new_address_space();
  if (new_pml4 == 0) {
    kfree(filebuf);
    f->rax = (uint64_t)-1;
    return;
  }

  struct elf_load_result elf;
  bool loaded = elf_load(new_pml4, filebuf, size, &elf);
  kfree(filebuf);
  if (!loaded) {
    vmm_free_user_space(new_pml4);
    f->rax = (uint64_t)-1;
    return;
  }

  uint64_t stack_top = USER_STACK_TOP;
  for (uint64_t i = 0; i < USER_STACK_PAGES; i++) {
    uint64_t phys = pmm_alloc_page();
    if (phys == 0) {
      vmm_free_user_space(new_pml4);
      f->rax = (uint64_t)-1;
      return;
    }
    uint64_t va = stack_top - (i + 1) * PAGE_SIZE;
    vmm_map_page_in(new_pml4, va, phys, VMM_WRITABLE | VMM_USER | VMM_NX);
  }

  /* Note: exec() does NOT touch uid -- the credential belongs to the
   * process, not the image, exactly like a real exec() with no setuid
   * bit involved. */
  struct task *t = this_cpu()->current_task;
  uint64_t old_pml4 = t->cr3_phys;

  write_cr3(new_pml4);
  t->cr3_phys = new_pml4;
  t->brk_start = ALIGN_UP(elf.highest_vaddr, PAGE_SIZE);
  t->brk = t->brk_start;

  strncpy(t->name, path, sizeof(t->name) - 1);
  t->name[sizeof(t->name) - 1] = '\0';

  vmm_free_user_space(old_pml4);

  f->rip = elf.entry;
  f->rsp = stack_top;
}

static void sys_split_impl(struct interrupt_frame *f) {
  uint64_t trampoline_va = f->rdi;
  uint64_t entry_va = f->rsi;
  uint64_t arg = f->rdx;

  if (!user_range_ok(trampoline_va, 1) || !user_range_ok(entry_va, 1)) {
    f->rax = (uint64_t)-1;
    return;
  }

  struct task *parent = this_cpu()->current_task;

  uint64_t new_pml4 = vmm_copy_address_space(parent->cr3_phys);
  if (new_pml4 == 0) {
    f->rax = (uint64_t)-1;
    return;
  }

  struct task *child =
      task_create_user(parent->name, new_pml4, trampoline_va, USER_STACK_TOP,
                       arg, entry_va, parent->uid); /* split()'s child
                                                        inherits the
                                                        SAME clearance
                                                        as the parent --
                                                        it's still the
                                                        same logical
                                                        process, just a
                                                        second thread of
                                                        control */
  if (child == NULL) {
    vmm_free_user_space(new_pml4);
    f->rax = (uint64_t)-1;
    return;
  }

  child->brk_start = parent->brk_start;
  child->brk = parent->brk;
  child->parent = parent;
  ksnprintf(child->name, sizeof(child->name), "%s~%lu", parent->name,
            child->id);

  for (int i = 0; i < PROC_MAX_FDS; i++) {
    if (parent->fds[i] != NULL) {
      child->fds[i] = vfs_dup(parent->fds[i]);
      if (child->fds[i] == NULL) {
        kprintf("[split] pid %lu: couldn't duplicate fd %d into new pid "
                "%lu -- that fd starts closed there\n",
                parent->id, i, child->id);
      }
    }
  }

  task_publish(child);
  f->rax = child->id;
}

static void sys_wait_impl(struct interrupt_frame *f) {
  uint64_t pid = f->rdi;

  struct task *child = sched_find_waitable_task(pid);
  if (child == NULL) {
    f->rax = (uint64_t)-1;
    return;
  }

  int code = sched_wait_task(child);
  f->rax = (uint64_t)(int64_t)code;
}

static void sys_wait_any_impl(struct interrupt_frame *f) {
  uint64_t code_out_va = f->rdi;

  if (code_out_va != 0 && !user_range_ok(code_out_va, sizeof(int))) {
    f->rax = (uint64_t)-1;
    return;
  }

  int code = 0;
  int64_t pid = sched_wait_any(&code);
  if (pid < 0) {
    f->rax = (uint64_t)-1;
    return;
  }

  if (code_out_va != 0 &&
      copy_to_user((void *)code_out_va, &code, sizeof(code)) != 0) {
    f->rax = (uint64_t)-1;
    return;
  }
  f->rax = (uint64_t)pid;
}

/* kill() now enforces the same clearance boundary as every other
 * cross-task operation added this turn: root (uid 0) may signal
 * anything; anyone else may only signal a task with their OWN uid.
 * sched_find_waitable_task() already refuses anything that isn't
 * "waitable" (kernel tasks -- the shell, gautosave, blockdev init --
 * never are, see task_create()), so this closes the remaining gap:
 * two DIFFERENT ring-3 processes could previously kill() each other
 * freely just by knowing a pid, with no ownership concept at all. */
static void sys_kill_impl(struct interrupt_frame *f) {
  uint64_t pid = f->rdi;

  struct task *t = sched_find_waitable_task(pid);
  if (t == NULL) {
    f->rax = (uint64_t)-1;
    return;
  }

  struct task *caller = this_cpu()->current_task;
  if (caller->uid != 0 && caller->uid != t->uid) {
    /* Same -1 a nonexistent pid gets -- this ABI has no errno, so
     * there's no clean way to distinguish "no such task" from "not
     * yours" anyway, and not distinguishing them means a restricted
     * process can't use kill() to probe which pids exist elsewhere
     * in the system. */
    f->rax = (uint64_t)-1;
    return;
  }

  sched_kill_task(t);
  f->rax = 0;
}

struct ps_lookup_ctx {
  uint32_t target_index;
  uint32_t seen;
  bool found;
  nx_task_info_t info;
};

static void ps_lookup_one(struct task *t, void *arg) {
  struct ps_lookup_ctx *ctx = (struct ps_lookup_ctx *)arg;
  if (ctx->found) {
    return; /* sched_for_each_task() has no early-exit -- just skip
                the rest of the walk once we've found our target */
  }
  if (ctx->seen == ctx->target_index) {
    ctx->info.pid = t->id;
    strncpy(ctx->info.name, t->name, sizeof(ctx->info.name) - 1);
    ctx->info.name[sizeof(ctx->info.name) - 1] = '\0';
    ctx->info.state = (uint32_t)t->state;
    ctx->info.is_user = t->is_user ? 1 : 0;
    ctx->info.uid = t->uid;
    ctx->found = true;
  }
  ctx->seen++;
}

static void sys_ps_impl(struct interrupt_frame *f) {
  uint32_t index = (uint32_t)f->rdi;
  uint64_t out_va = f->rsi;

  if (!user_range_ok(out_va, sizeof(nx_task_info_t))) {
    f->rax = (uint64_t)-1;
    return;
  }

  struct ps_lookup_ctx ctx = {.target_index = index, .seen = 0, .found = false};
  sched_for_each_task(ps_lookup_one, &ctx);

  if (!ctx.found) {
    f->rax = (uint64_t)-1;
    return;
  }

  if (copy_to_user((void *)out_va, &ctx.info, sizeof(ctx.info)) != 0) {
    f->rax = (uint64_t)-1;
    return;
  }
  f->rax = 0;
}

static void syscall_dispatch(struct interrupt_frame *f) {
  struct task *self = this_cpu()->current_task;
  if (__atomic_load_n(&self->kill_requested, __ATOMIC_ACQUIRE)) {
    self->exit_code = 137;
    task_exit(); /* never returns */
  }
  switch (f->rax) {
  case SYS_exit:
    sys_exit_impl(f);
    break;
  case SYS_write:
    sys_write_impl(f);
    break;
  case SYS_read:
    sys_read_impl(f);
    break;
  case SYS_open:
    sys_open_impl(f);
    break;
  case SYS_close:
    sys_close_impl(f);
    break;
  case SYS_getpid:
    sys_getpid_impl(f);
    break;
  case SYS_sleep_ms:
    sys_sleep_ms_impl(f);
    break;
  case SYS_yield:
    sys_yield_impl(f);
    break;
  case SYS_brk:
    sys_brk_impl(f);
    break;
  case SYS_readdir:
    sys_readdir_impl(f);
    break;
  case SYS_uptime_ms:
    sys_uptime_ms_impl(f);
    break;
  case SYS_spawn:
    sys_spawn_impl(f);
    break;
  case SYS_exec:
    sys_exec_impl(f);
    break;
  case SYS_split:
    sys_split_impl(f);
    break;
  case SYS_wait:
    sys_wait_impl(f);
    break;
  case SYS_wait_any:
    sys_wait_any_impl(f);
    break;
  case SYS_ps:
    sys_ps_impl(f);
    break;
  case SYS_kill:
    sys_kill_impl(f);
    break;
  case SYS_getuid:
    sys_getuid_impl(f);
    break;
  case SYS_setuid:
    sys_setuid_impl(f);
    break;
  default:
    kprintf("[syscall] pid %lu made unknown syscall %lu\n",
            this_cpu()->current_task->id, f->rax);
    f->rax = (uint64_t)-1;
    break;
  }
}

void syscall_init(void) {
  register_interrupt_handler(VEC_SYSCALL, syscall_dispatch);
}
