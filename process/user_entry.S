.section .text.user_entry, "ax"
.global user_enter
.global user_exit_return
.extern user_return_pc

user_enter:
    /* x0 = EL0 entry, x1 = EL0 stack top. */
    adrp x9, user_return_pc
    add x9, x9, :lo12:user_return_pc
    str x30, [x9]

    msr sp_el0, x1

    /* Return to EL0t, mask asynchronous exceptions during this single task. */
    mov x10, #0x3c0
    msr spsr_el1, x10
    msr elr_el1, x0

    mov x2, xzr
    mov x3, xzr
    mov x4, xzr
    mov x5, xzr
    mov x6, xzr
    mov x7, xzr
    mov x8, xzr
    mov x9, xzr
    mov x30, xzr

    isb
    eret

user_exit_return:
    adrp x16, user_return_pc
    add x16, x16, :lo12:user_return_pc
    ldr x16, [x16]
    cbz x16, 1f
    br x16
1:
    wfe
    b 1b
