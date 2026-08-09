#include "cpu/syscall.h"
#include "abi/syscall_nr.h"
#include "cpu/cpu.h"
#include "cpu/isr.h"
#include "cpu/vectors.h"
#include "debug/log.h"
#include "drivers/keyboard.h"
#include "fs/vfs.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "proc/process.h"
#include "sched/sched.h"
#include "time/timer.h"

/*
 * The syscall ABI: `int $0x80`, number in rax, args in rdi/rsi/rdx/r10/
 * r8, return value in rax. isr_stubs.S has already pushed every GPR
 * into `frame` by the time syscall_dispatch() runs (see cpu/isr.c), so
 * this is just an ordinary registered VEC_SYSCALL handler -- no new
 * assembly needed anywhere, only the DPL=3 IDT gate set up in
 * cpu/idt.c.
 *
 * A known, deliberate gap: every syscall that touches a user-supplied
 * pointer only does a shallow range check (user_range_ok(), below) --
 * "is this a plausible canonical low-half address" -- not a real
 * per-page walk of the caller's own page tables with #PF recovery
 * (a proper copy_from_user()/copy_to_user()). A user program that
 * hands the kernel a syntactically-valid-but-unmapped pointer will
 * take an unhandled page fault *in the kernel*, which today means a
 * panic instead of just killing that one process. Worth knowing if
 * you're about to run something you don't trust.
 */

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

static void sys_write_impl(struct interrupt_frame *f) {
    int fd = (int)f->rdi;
    uint64_t buf = f->rsi;
    uint64_t len = f->rdx;

    if (!user_range_ok(buf, len)) {
        f->rax = (uint64_t)-1;
        return;
    }

    if (fd == STDOUT_FILENO || fd == STDERR_FILENO) {
        /* kprintf's internal buffer is bounded and this isn't a format
         * string, so hand it over in small NUL-terminated chunks. */
        char chunk[128];
        uint64_t done = 0;
        while (done < len) {
            uint64_t n = MIN(len - done, sizeof(chunk) - 1);
            memcpy(chunk, (const void *)(buf + done), n);
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
    f->rax = vfs_write(t->fds[fd], (const void *)buf, len);
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
        uint8_t *dst = (uint8_t *)buf;
        uint64_t n = 0;
        while (n < len) {
            char c = keyboard_getc(); /* blocks -- fine, we're not holding anything */
            dst[n++] = (uint8_t)c;
            if (c == '\n') {
                break;
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
    f->rax = vfs_read(t->fds[fd], (void *)buf, len);
}

static void sys_open_impl(struct interrupt_frame *f) {
    uint64_t path_va = f->rdi;
    if (!user_range_ok(path_va, 1)) {
        f->rax = (uint64_t)-1;
        return;
    }

    char path[256];
    strncpy(path, (const char *)path_va, sizeof(path) - 1);
    path[sizeof(path) - 1] = '\0';

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
    if (!vfs_open(path, &file)) {
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
                f->rax = t->brk; /* out of memory: brk stays where it was */
                return;
            }
            vmm_map_page_in(t->cr3_phys, va, phys,
                             VMM_PRESENT | VMM_WRITABLE | VMM_USER | VMM_NX);
        }
    }
    /* Shrinking just moves the pointer back without unmapping -- none
     * of v1's demo programs ever shrink, and freeing those pages
     * properly is easy to add later (vmm_free_user_space() already
     * shows the walk). */

    t->brk = req;
    f->rax = t->brk;
}

static void sys_readdir_impl(struct interrupt_frame *f) {
    uint64_t path_va = f->rdi;
    uint32_t index = (uint32_t)f->rsi;
    uint64_t out_va = f->rdx;
    uint64_t out_len = f->r10;

    if (!user_range_ok(path_va, 1) || !user_range_ok(out_va, out_len)) {
        f->rax = (uint64_t)-1;
        return;
    }

    char path[256];
    strncpy(path, (const char *)path_va, sizeof(path) - 1);
    path[sizeof(path) - 1] = '\0';

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
        memcpy((void *)out_va, name, n);
        ((char *)out_va)[n] = '\0';
    }
    f->rax = 0;
}

static void syscall_dispatch(struct interrupt_frame *f) {
    switch (f->rax) {
    case SYS_exit:      sys_exit_impl(f); break;
    case SYS_write:     sys_write_impl(f); break;
    case SYS_read:      sys_read_impl(f); break;
    case SYS_open:      sys_open_impl(f); break;
    case SYS_close:     sys_close_impl(f); break;
    case SYS_getpid:    sys_getpid_impl(f); break;
    case SYS_sleep_ms:  sys_sleep_ms_impl(f); break;
    case SYS_yield:     sys_yield_impl(f); break;
    case SYS_brk:       sys_brk_impl(f); break;
    case SYS_readdir:   sys_readdir_impl(f); break;
    case SYS_uptime_ms: sys_uptime_ms_impl(f); break;
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
