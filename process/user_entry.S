/*
 * MIT License
 *
 * Copyright (c) 2026 Rio okta kusuma
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files, to deal in the Software
 * without restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies of the
 * Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

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

    /* Return to EL0t with IRQ unmasked (I=0) so the 100 Hz timer tick keeps
       running while the program executes. Debug, SError and FIQ stay masked
       (D=1 A=1 F=1 -> 0x340). */
    mov x10, #0x340
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

/* user_resume(ctx, kernel_stack_top): load a full ProcessCpuContext
 * (x0-x30 @0, elr @248, spsr @256, sp_el0 @264) and eret into EL0.
 * SP_EL1 is reset to the thread's kernel stack top, so every exception taken
 * from EL0 starts on an empty kernel stack. Never returns. */
.global user_resume
user_resume:
    msr daifset, #0x2
    ldr x2, [x0, #248]
    msr elr_el1, x2
    ldr x2, [x0, #256]
    msr spsr_el1, x2
    ldr x2, [x0, #264]
    msr sp_el0, x2
    mov x9, x1                     /* new kernel stack top */
    mov x8, x0
    ldp x2, x3, [x8, #16]
    ldp x4, x5, [x8, #32]
    ldp x6, x7, [x8, #48]
    ldp x10, x11, [x8, #80]
    ldp x12, x13, [x8, #96]
    ldp x14, x15, [x8, #112]
    ldp x16, x17, [x8, #128]
    ldp x18, x19, [x8, #144]
    ldp x20, x21, [x8, #160]
    ldp x22, x23, [x8, #176]
    ldp x24, x25, [x8, #192]
    ldp x26, x27, [x8, #208]
    ldp x28, x29, [x8, #224]
    ldr x30, [x8, #240]
    ldp x0, x1, [x8, #0]
    mov sp, x9
    ldr x9, [x8, #72]
    ldr x8, [x8, #64]
    isb
    eret
