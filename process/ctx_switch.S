/* Kernel thread context switch (AArch64).
   Saves/restores the AAPCS64 callee-saved registers x19-x30 plus SP.
   Placed in plain .text so linker.ld's *(.text) picks it up. */
.section .text, "ax"
.global thread_context_switch
.global thread_trampoline
.extern thread_entry_hook
.extern thread_exit

/* void thread_context_switch(uint64_t* old_sp_out /*x0*/, uint64_t new_sp /*x1*/) */
thread_context_switch:
    sub sp, sp, #96
    stp x19, x20, [sp, #0]
    stp x21, x22, [sp, #16]
    stp x23, x24, [sp, #32]
    stp x25, x26, [sp, #48]
    stp x27, x28, [sp, #64]
    stp x29, x30, [sp, #80]

    mov x9, sp
    str x9, [x0]

    mov sp, x1
    ldp x19, x20, [sp, #0]
    ldp x21, x22, [sp, #16]
    ldp x23, x24, [sp, #32]
    ldp x25, x26, [sp, #48]
    ldp x27, x28, [sp, #64]
    ldp x29, x30, [sp, #80]
    add sp, sp, #96
    ret

/* First code run by a new thread: x19 = entry, x20 = argument. */
thread_trampoline:
    bl thread_entry_hook
    mov x0, x20
    blr x19
    bl thread_exit
1:
    wfe
    b 1b
