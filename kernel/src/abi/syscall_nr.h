#ifndef NEXUS_SYSCALL_NR_H
#define NEXUS_SYSCALL_NR_H

/*
 * Nexus's syscall ABI: the numbers and flag bits that ring-3 code and
 * the kernel's syscall dispatcher (cpu/syscall.c) both need to agree
 * on. Deliberately just #defines -- no kernel-only types -- so this
 * exact file can be (and is) copied verbatim into userland/include/.
 * If you change something here, change it there too.
 *
 * Calling convention: `int $0x80`, syscall number in rax, up to 5 args
 * in rdi, rsi, rdx, r10, r8 (in that order -- r10 instead of rcx even
 * though nothing here clobbers rcx, just to look like the real thing).
 * Return value in rax; negative means error (there's no errno yet --
 * you get -1 and that's the whole story, v1).
 */

#define SYS_exit 0     // exit(code)                    -> never returns
#define SYS_write 1    // write(fd, buf, len)            -> bytes written, or -1
#define SYS_read 2     // read(fd, buf, len)              -> bytes read, or -1
#define SYS_open 3     // open(path, flags)                -> fd, or -1
#define SYS_close 4    // close(fd)                          -> 0, or -1
#define SYS_getpid 5   // getpid()                             -> pid
#define SYS_sleep_ms 6 // sleep_ms(ms)                           -> 0
#define SYS_yield 7    // yield()                                 -> 0
#define SYS_brk 8 // brk(new_brk_or_0)                        -> current brk
#define SYS_readdir 9 // readdir(path, index, name_out, out_len)   -> 0, or -1 at EOF
#define SYS_uptime_ms 10 // uptime_ms()                                 -> ms since boot
#define SYS_spawn 11 // spawn(path)                                 -> pid, or -1
#define SYS_wait 12 // wait(pid)                                    -> exit code, or -1
#define SYS_ps 13 // ps(index, nx_task_info_t *out)                 -> 0, or -1 at EOF
#define SYS_kill 14
#define SYS_COUNT 15

/* open() flags -- deliberately tiny; no O_APPEND/O_TRUNC/etc yet. */
#define O_RDONLY 0x0
#define O_WRONLY 0x1
#define O_RDWR 0x2
#define O_CREAT 0x4
#define O_TRUNC 0x8

/* fixed well-known file descriptors, same numbering as everywhere else */
#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#endif /* NEXUS_SYSCALL_NR_H */
