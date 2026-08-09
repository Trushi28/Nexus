.section .text
.global _start
_start:
    xor %rbp, %rbp      /* mark the bottom of the call stack, ABI convention */
    call main
    mov %eax, %edi       /* main()'s return value becomes the exit code (SYS_exit's arg) */
    mov $0, %eax         /* SYS_exit == 0 -- see include/syscall_nr.h */
    int $0x80
1:  jmp 1b               /* unreachable: exit() never returns */

.section .note.GNU-stack, "", @progbits
