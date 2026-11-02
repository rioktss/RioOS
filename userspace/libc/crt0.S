.section .text
.global _start
_start:
    mov x29, xzr
    mov x30, xzr
    bl __libc_start
1:  b 1b
