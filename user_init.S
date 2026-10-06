.section .text
.global _start
.type _start, %function
_start:
    /* SYS_WRITE: x0=user string pointer, x8=1 */
    adrp x0, msg
    add  x0, x0, :lo12:msg
    mov  x8, #1
    svc  #0

    /* SYS_UPTIME */
    mov  x8, #2
    svc  #0

    /* SYS_GETPID */
    mov  x8, #3
    svc  #0

    /* SYS_EXIT(0) */
    mov  x0, #0
    mov  x8, #6
    svc  #0

1:
    wfe
    b 1b

.section .rodata
.align 3
msg:
    .asciz "[EL0] MyKernel userspace is alive.\r\n"
