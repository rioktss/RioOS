.section .text.user_entry, "ax"
.global user_enter
.global user_exit_return

/* user_enter(entry, stack_top)
 *
 * Called from C++ like any function, so it must preserve the AAPCS64
 * callee-saved registers (x19-x28, x29, x30) for its caller. EL0 code is free
 * to clobber all of them, so they are parked on the kernel (SP_EL1) stack and
 * put back by user_exit_return when the process exits or faults. SP_EL1 is not
 * touched while EL0 runs, so it is exactly the same on the way back.
 */
user_enter:
    /* x0 = EL0 entry, x1 = EL0 stack top. */
    stp x29, x30, [sp, #-96]!
    stp x19, x20, [sp, #16]
    stp x21, x22, [sp, #32]
    stp x23, x24, [sp, #48]
    stp x25, x26, [sp, #64]
    stp x27, x28, [sp, #80]

    msr sp_el0, x1

    /* Return to EL0t, mask asynchronous exceptions during this single task. */
    mov x10, #0x3c0
    msr spsr_el1, x10
    msr elr_el1, x0

    mov x0, xzr
    mov x1, xzr
    mov x2, xzr
    mov x3, xzr
    mov x4, xzr
    mov x5, xzr
    mov x6, xzr
    mov x7, xzr
    mov x8, xzr
    mov x9, xzr
    mov x10, xzr
    mov x19, xzr
    mov x20, xzr
    mov x21, xzr
    mov x22, xzr
    mov x23, xzr
    mov x24, xzr
    mov x25, xzr
    mov x26, xzr
    mov x27, xzr
    mov x28, xzr
    mov x29, xzr
    mov x30, xzr

    isb
    eret

/* Reached via ERET to EL1h (SPSR 0x3C5) from the exit syscall or from a fatal
 * EL0 fault. SP_EL1 is back to the value it had right after the stp in
 * user_enter, so unwind that frame and return to user_enter's caller. */
user_exit_return:
    ldp x19, x20, [sp, #16]
    ldp x21, x22, [sp, #32]
    ldp x23, x24, [sp, #48]
    ldp x25, x26, [sp, #64]
    ldp x27, x28, [sp, #80]
    ldp x29, x30, [sp], #96
    ret
