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

.section .text.vectors, "ax"
.align 11
.global exception_vectors_el1

.macro VECTOR target
    b \target
    .space 124
.endm

/* Exactly 16 entries x 128 bytes = 2048 bytes. */
exception_vectors_el1:
    /* Current EL with SP_EL0: synchronous, IRQ, FIQ, SError. */
    VECTOR sync_el1_entry
    VECTOR irq_el1_entry
    VECTOR irq_unhandled_entry
    VECTOR irq_unhandled_entry

    /* Current EL with SP_ELx: synchronous, IRQ, FIQ, SError. */
    VECTOR sync_el1_entry
    VECTOR irq_el1_entry
    VECTOR irq_unhandled_entry
    VECTOR irq_unhandled_entry

    /* Lower EL using AArch64: synchronous, IRQ, FIQ, SError. */
    VECTOR sync_el0_entry
    VECTOR irq_el0_entry
    VECTOR irq_unhandled_entry
    VECTOR irq_unhandled_entry

    /* Lower EL using AArch32: unsupported in this kernel. */
    VECTOR irq_unhandled_entry
    VECTOR irq_unhandled_entry
    VECTOR irq_unhandled_entry
    VECTOR irq_unhandled_entry

.section .text.exception_handlers, "ax"
.global sync_el1_entry
.global sync_el0_entry
.global exception_restore_el1
.global irq_unhandled_entry
.global irq_el1_entry
.global irq_el0_entry

.macro SAVE_EXCEPTION_FRAME
    /* 31 GPRs + ELR + SPSR + ESR + FAR + padding = 288 bytes. */
    sub sp, sp, #288

    stp x0, x1, [sp, #0]
    stp x2, x3, [sp, #16]
    stp x4, x5, [sp, #32]
    stp x6, x7, [sp, #48]
    stp x8, x9, [sp, #64]
    stp x10, x11, [sp, #80]
    stp x12, x13, [sp, #96]
    stp x14, x15, [sp, #112]
    stp x16, x17, [sp, #128]
    stp x18, x19, [sp, #144]
    stp x20, x21, [sp, #160]
    stp x22, x23, [sp, #176]
    stp x24, x25, [sp, #192]
    stp x26, x27, [sp, #208]
    stp x28, x29, [sp, #224]
    str x30, [sp, #240]

    mrs x0, elr_el1
    str x0, [sp, #248]
    mrs x0, spsr_el1
    str x0, [sp, #256]
    mrs x0, esr_el1
    str x0, [sp, #264]
    mrs x0, far_el1
    str x0, [sp, #272]
.endm

sync_el1_entry:
    SAVE_EXCEPTION_FRAME
    bl bkl_enter_el1
    str x0, [sp, #280]
    mov x0, sp
    bl exception_sync_el1
    ldr x0, [sp, #280]
    bl bkl_leave_el1
    b exception_restore_el1

sync_el0_entry:
    SAVE_EXCEPTION_FRAME
    bl bkl_enter_user
    mov x0, sp
    bl exception_sync_el0
    mov x0, sp
    bl task_deliver_signals
    mov x0, sp
    bl bkl_leave_exception
    b exception_restore_el1

irq_el1_entry:
    SAVE_EXCEPTION_FRAME
    bl bkl_enter_el1
    str x0, [sp, #280]
    mov x0, sp
    bl interrupt_dispatch
    ldr x0, [sp, #280]
    bl bkl_leave_el1
    b exception_restore_el1

irq_el0_entry:
    SAVE_EXCEPTION_FRAME
    bl bkl_enter_user
    mov x0, sp
    bl interrupt_dispatch
    bl thread_user_preempt
    mov x0, sp
    bl task_deliver_signals
    mov x0, sp
    bl bkl_leave_exception
    b exception_restore_el1

exception_restore_el1:
    /* Restore architectural exception state first. */
    ldr x0, [sp, #248]
    msr elr_el1, x0

    ldr x0, [sp, #256]
    msr spsr_el1, x0

    /* Restore all GPRs exactly as they were at exception entry. */
    ldp x28, x29, [sp, #224]
    ldr x30, [sp, #240]
    ldp x26, x27, [sp, #208]
    ldp x24, x25, [sp, #192]
    ldp x22, x23, [sp, #176]
    ldp x20, x21, [sp, #160]
    ldp x18, x19, [sp, #144]
    ldp x16, x17, [sp, #128]
    ldp x14, x15, [sp, #112]
    ldp x12, x13, [sp, #96]
    ldp x10, x11, [sp, #80]
    ldp x8, x9, [sp, #64]
    ldp x6, x7, [sp, #48]
    ldp x4, x5, [sp, #32]
    ldp x2, x3, [sp, #16]
    ldp x0, x1, [sp, #0]

    add sp, sp, #288
    eret

irq_unhandled_entry:
    b exception_irq_unhandled
