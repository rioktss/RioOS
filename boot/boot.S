.section .text.boot
.global _start
.type _start, %function
.extern kernel_main

/* Very early diagnostic: direct PL011 write, before any C/C++ code. */
.macro EARLY_PUTC reg
    mov \reg, #0x9000000
    mov w2, #\reg
.endm

_start:
    /* QEMU virt bare-metal ELF boots provide the generated DTB at the
       start of RAM. Save that address before BSS is cleared. */
    mov x27, #0x40000000

    /* QEMU virt PL011: direct 'B' tells us execution reached reset entry. */
    mov x9, #0x9000000
    mov w10, #'B'
    str w10, [x9]

    ldr x0, =stack_top
    mov sp, x0

    /* Clear BSS before any C/C++ code executes. */
    ldr x1, =__bss_start
    ldr x2, =__bss_end
    mov x0, #0

clear_bss:
    cmp x1, x2
    b.hs bss_done
    str x0, [x1], #8
    b clear_bss

bss_done:
    ldr x0, =boot_dtb_ptr
    str x27, [x0]

    mrs x0, CurrentEL
    lsr x0, x0, #2
    and x0, x0, #3
    cmp x0, #2
    b.eq boot_drop_el2
    cmp x0, #1
    b.eq boot_el1
    b boot_hang

boot_drop_el2:
    /* Allow EL1 access to the generic timer counters. */
    mov x1, #3
    msr cnthctl_el2, x1

    /* Force lower exception level into AArch64. */
    mrs x1, hcr_el2
    mov x2, #0x80000000
    orr x1, x1, x2
    msr hcr_el2, x1

    ldr x1, =stack_top
    msr sp_el1, x1
    ldr x1, =boot_el1
    msr elr_el2, x1
    /* EL1h, DAIF masked. */
    mov x1, #0x3C5
    msr spsr_el2, x1
    isb
    eret

boot_el1:
    mov x1, #1
    msr spsel, x1
    isb

    /* Second diagnostic: EL1 was reached. */
    mov x9, #0x9000000
    mov w10, #'E'
    str w10, [x9]

    bl kernel_main

boot_hang:
    wfe
    b boot_hang

.section .stack, "aw", %nobits
.align 12
stack:
    .skip 16384
stack_top:
