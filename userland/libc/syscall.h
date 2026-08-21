#ifndef NEXUS_USER_SYSCALL_H
#define NEXUS_USER_SYSCALL_H

#include "syscall_nr.h"

/* Nexus's syscall ABI (see syscall_nr.h): `int $0x80`, number in
 * rax, up to 5 args in rdi/rsi/rdx/r10/r8, return value in rax. */
static inline long u_syscall(long nr, long a1, long a2, long a3, long a4, long a5) {
    long ret;
    register long r10 asm("r10") = a4;
    register long r8  asm("r8")  = a5;
    asm volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(nr), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8)
        : "memory"
    );
    return ret;
}

#define SC0(n)             u_syscall(n, 0, 0, 0, 0, 0)
#define SC1(n, a)          u_syscall(n, (long)(a), 0, 0, 0, 0)
#define SC2(n, a, b)       u_syscall(n, (long)(a), (long)(b), 0, 0, 0)
#define SC3(n, a, b, c)    u_syscall(n, (long)(a), (long)(b), (long)(c), 0, 0)
#define SC4(n, a, b, c, d) u_syscall(n, (long)(a), (long)(b), (long)(c), (long)(d), 0)

#endif /* NEXUS_USER_SYSCALL_H */
