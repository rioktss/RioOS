.text
.global _start
.align 2
_start:
    bl nano_main
    mov x8, #93
    svc #0

sys_open: mov x8, #2; svc #0; ret
sys_close: mov x8, #3; svc #0; ret
sys_read: mov x8, #1; svc #0; ret
sys_write: mov x8, #0; svc #0; ret
sys_getc: mov x8, #4; svc #0; ret
sys_puts: mov x8, #5; svc #0; ret

.rodata
msg_banner:.asciz "\n=== RioOS nano v1.0 ===\nFile: /storage/home/hello | CTRL+S save | CTRL+X exit\n------------------------\n"
msg_saved:.asciz "\n[Saved!]\n"
msg_exit:.asciz "\n[Exit]\n"
filename:.asciz "/storage/home/hello"

.data
.align 12
buffer:.skip 4096
len:.quad 0
cursor:.quad 0

.text
.align 2
nano_main:
    stp x29, x30, [sp, #-16]!

    adrp x0, filename
    add x0, x0, :lo12:filename
    mov x1, #0
    bl sys_open
    cmp x0, #0
    blt load_empty
    mov x19, x0
    adrp x1, buffer
    add x1, x1, :lo12:buffer
    mov x2, #4096
    bl sys_read
    adrp x1, len
    add x1, x1, :lo12:len
    str x0, [x1]
    adrp x1, cursor
    add x1, x1, :lo12:cursor
    str x0, [x1]
    mov x0, x19
    bl sys_close
    b draw

load_empty:
    adrp x0, len
    add x0, x0, :lo12:len
    str xzr, [x0]
    adrp x0, cursor
    add x0, x0, :lo12:cursor
    str xzr, [x0]

draw:
    adrp x0, msg_banner
    add x0, x0, :lo12:msg_banner
    bl sys_puts
    adrp x0, buffer
    add x0, x0, :lo12:buffer
    adrp x1, len
    add x1, x1, :lo12:len
    ldr x1, [x1]
    mov x19, #0
draw_loop:
    adrp x2, len
    add x2, x2, :lo12:len
    ldr x2, [x2]
    cmp x19, x2
    bge input_loop
    adrp x2, buffer
    add x2, x2, :lo12:buffer
    ldrb w0, [x2, x19]
    sub sp, sp, #16
    strb w0, [sp]
    mov w1, #0
    strb w1, [sp, #1]
    mov x0, sp
    bl sys_puts
    add sp, sp, #16
    add x19, x19, #1
    b draw_loop

input_loop:
    bl sys_getc
    cmp x0, #19
    b.eq do_save
    cmp x0, #24
    b.eq do_exit
    cmp x0, #8
    b.eq do_backspace
    cmp x0, #127
    b.eq do_backspace
    cmp x0, #13
    b.eq handle_enter
    cmp x0, #32
    blt input_loop
    cmp x0, #126
    bgt input_loop
    adrp x1, len
    add x1, x1, :lo12:len
    ldr x2, [x1]
    cmp x2, #4095
    bge input_loop
    adrp x3, cursor
    add x3, x3, :lo12:cursor
    ldr x4, [x3]
    adrp x5, buffer
    add x5, x5, :lo12:buffer
    mov x6, x2
sr: cmp x6, x4; ble ins; sub x7, x6, #1; ldrb w8, [x5, x7]; strb w8, [x5, x6]; sub x6, x6, #1; b sr
ins: strb w0, [x5, x4]; add x2, x2, #1; str x2, [x1]; add x4, x4, #1; str x4, [x3]; b draw
handle_enter:
    mov x0, #10
    adrp x1, len; add x1, x1, :lo12:len; ldr x2, [x1]; cmp x2, #4095; bge input_loop
    adrp x3, cursor; add x3, x3, :lo12:cursor; ldr x4, [x3]
    adrp x5, buffer; add x5, x5, :lo12:buffer; mov x6, x2
sr2: cmp x6, x4; ble ins2; sub x7, x6, #1; ldrb w8, [x5, x7]; strb w8, [x5, x6]; sub x6, x6, #1; b sr2
ins2: strb w0, [x5, x4]; add x2, x2, #1; str x2, [x1]; add x4, x4, #1; str x4, [x3]; b draw
do_backspace:
    adrp x0, cursor; add x0, x0, :lo12:cursor; ldr x1, [x0]; cbz x1, input_loop
    adrp x2, len; add x2, x2, :lo12:len; ldr x3, [x2]
    adrp x4, buffer; add x4, x4, :lo12:buffer; sub x1, x1, #1; mov x5, x1
sl: cmp x5, x3; bge sld; add x6, x5, #1; ldrb w7, [x4, x6]; strb w7, [x4, x5]; add x5, x5, #1; b sl
sld: sub x3, x3, #1; str x3, [x2]; str x1, [x0]; b draw
do_save:
    adrp x0, filename; add x0, x0, :lo12:filename; mov x1, #5; bl sys_open
    cmp x0, #0; blt input_loop; mov x19, x0
    adrp x1, buffer; add x1, x1, :lo12:buffer
    adrp x2, len; add x2, x2, :lo12:len; ldr x2, [x2]
    mov x8, #0; svc #0
    mov x0, x19; bl sys_close
    adrp x0, msg_saved; add x0, x0, :lo12:msg_saved; bl sys_puts; b input_loop
do_exit:
    adrp x0, msg_exit; add x0, x0, :lo12:msg_exit; bl sys_puts
    ldp x29, x30, [sp], #16; ret