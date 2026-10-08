/*
 * RioOS nano v1.2 - tiny EL0 text editor.
 *
 * Syscall ABI (see include/syscall.h): number in x8, args in x0..x2,
 * result in x0.
 *   1  SYS_WRITE       x0 = NUL-terminated string
 *   6  SYS_EXIT        x0 = exit code
 *   8  SYS_WRITE_BUF   x0 = buf, x1 = len
 *  10  SYS_READ        x0 = buf, x1 = len   (non-blocking, returns count)
 *  12  SYS_FILE_READ   x0 = path, x1 = buf, x2 = cap   -> bytes or -errno
 *  13  SYS_FILE_WRITE  x0 = path, x1 = buf, x2 = len   -> bytes or -errno
 *  14  SYS_GETARG      x0 = buf, x1 = cap  -> length of "nano <file>" argument
 *
 * File paths are resolved by the kernel relative to the shell's current
 * directory, so `nano test.txt` saves where the shell currently is.
 *
 * Keys: printable = insert at end, Enter = newline, Backspace = delete,
 *       CTRL+S or CTRL+O = save, CTRL+X = exit.
 */

    .text
    .global _start
    .align 2
_start:
    bl nano_main
    mov x8, #6
    svc #0
hang:
    wfe
    b hang

/* ---- raw syscall wrappers (arguments already in place) ---- */
sys_puts:
    mov x8, #1
    svc #0
    ret

sys_write_buf:
    mov x8, #8
    svc #0
    ret

sys_read:
    mov x8, #10
    svc #0
    ret

sys_file_read:
    mov x8, #12
    svc #0
    ret

sys_file_write:
    mov x8, #13
    svc #0
    ret

sys_getarg:
    mov x8, #14
    svc #0
    ret

/* ---- put_char: w0 = byte, writes it to the console ---- */
put_char:
    stp x29, x30, [sp, #-32]!
    mov x29, sp
    strb w0, [sp, #16]
    add x0, sp, #16
    mov x1, #1
    bl sys_write_buf
    ldp x29, x30, [sp], #32
    ret

/* ---- put_char_nl: like put_char but LF becomes CR+LF ---- */
put_char_nl:
    stp x29, x30, [sp, #-32]!
    mov x29, sp
    str x0, [sp, #16]
    cmp w0, #10
    b.ne pcn_plain
    mov w0, #13
    bl put_char
pcn_plain:
    ldr x0, [sp, #16]
    bl put_char
    ldp x29, x30, [sp], #32
    ret

/* ---- get_char: polls until one byte arrives, returns it in x0 ---- */
get_char:
    stp x29, x30, [sp, #-32]!
    mov x29, sp
gc_retry:
    add x0, sp, #16
    mov x1, #1
    bl sys_read
    cmp x0, #1
    b.eq gc_got
    yield
    b gc_retry
gc_got:
    ldrb w0, [sp, #16]
    ldp x29, x30, [sp], #32
    ret

/* ---- main ---- */
nano_main:
    stp x29, x30, [sp, #-32]!
    stp x19, x20, [sp, #16]
    mov x29, sp

    /* Fetch the file name given on the command line ("nano <file>").
       Direct `exec /bin/nano.elf` is also valid: use a safe default file
       name instead of entering the old error path. */
    adrp x0, fname
    add x0, x0, :lo12:fname
    mov x1, #128
    bl sys_getarg
    cmp x0, #0
    b.lt arg_error
    b.ne have_name
    adrp x0, fname
    add x0, x0, :lo12:fname
    adrp x1, default_name
    add x1, x1, :lo12:default_name
    mov x2, #16
copy_default_name:
    ldrb w3, [x1], #1
    strb w3, [x0], #1
    cbz w3, have_name
    subs x2, x2, #1
    b.ne copy_default_name
    b arg_error

have_name:

    /* Load the file (missing file => empty buffer). */
    adrp x0, fname
    add x0, x0, :lo12:fname
    adrp x1, buffer
    add x1, x1, :lo12:buffer
    mov x2, #4095
    bl sys_file_read
    adrp x1, len
    add x1, x1, :lo12:len
    cmp x0, #0
    b.ge load_ok
    cmn x0, #27
    b.eq too_big
    b load_empty
load_ok:
    str x0, [x1]
    b redraw
load_empty:
    str xzr, [x1]

redraw:
    adrp x0, msg_title
    add x0, x0, :lo12:msg_title
    bl sys_puts
    adrp x0, fname
    add x0, x0, :lo12:fname
    bl sys_puts
    adrp x0, msg_keys
    add x0, x0, :lo12:msg_keys
    bl sys_puts
    mov x19, #0
redraw_loop:
    adrp x1, len
    add x1, x1, :lo12:len
    ldr x1, [x1]
    cmp x19, x1
    b.ge input_loop
    adrp x2, buffer
    add x2, x2, :lo12:buffer
    ldrb w0, [x2, x19]
    bl put_char_nl
    add x19, x19, #1
    b redraw_loop

input_loop:
    bl get_char
    cmp x0, #15
    b.eq do_save
    cmp x0, #19
    b.eq do_save
    cmp x0, #24
    b.eq do_exit
    cmp x0, #8
    b.eq do_backspace
    cmp x0, #127
    b.eq do_backspace
    cmp x0, #13
    b.eq do_enter
    cmp x0, #10
    b.eq do_enter
    cmp x0, #32
    b.lt input_loop
    cmp x0, #126
    b.gt input_loop
    mov x20, x0
    b append_char

do_enter:
    mov x20, #10

append_char:
    adrp x1, len
    add x1, x1, :lo12:len
    ldr x2, [x1]
    cmp x2, #4095
    b.ge input_loop
    adrp x3, buffer
    add x3, x3, :lo12:buffer
    strb w20, [x3, x2]
    add x2, x2, #1
    str x2, [x1]
    mov x0, x20
    bl put_char_nl
    b input_loop

do_backspace:
    adrp x1, len
    add x1, x1, :lo12:len
    ldr x2, [x1]
    cbz x2, input_loop
    sub x2, x2, #1
    str x2, [x1]
    adrp x3, buffer
    add x3, x3, :lo12:buffer
    ldrb w0, [x3, x2]
    cmp w0, #10
    b.eq redraw
    adrp x0, msg_bs
    add x0, x0, :lo12:msg_bs
    bl sys_puts
    b input_loop

do_save:
    adrp x0, fname
    add x0, x0, :lo12:fname
    adrp x1, buffer
    add x1, x1, :lo12:buffer
    adrp x2, len
    add x2, x2, :lo12:len
    ldr x2, [x2]
    bl sys_file_write
    cmp x0, #0
    b.lt save_failed
    adrp x0, msg_saved
    add x0, x0, :lo12:msg_saved
    bl sys_puts
    b input_loop
save_failed:
    adrp x0, msg_fail
    add x0, x0, :lo12:msg_fail
    bl sys_puts
    b input_loop

too_big:
    adrp x0, msg_big
    add x0, x0, :lo12:msg_big
    bl sys_puts
    mov x0, #1
    b leave

arg_error:
    adrp x0, msg_noname
    add x0, x0, :lo12:msg_noname
    bl sys_puts
    mov x0, #1
    b leave

do_exit:
    adrp x0, msg_exit
    add x0, x0, :lo12:msg_exit
    bl sys_puts
    mov x0, #0
leave:
    ldp x19, x20, [sp, #16]
    ldp x29, x30, [sp], #32
    ret

    .section .rodata
msg_title:
    .asciz "\033[2J\033[H=== RioOS nano v1.2 ===\r\nFile: "
msg_keys:
    .asciz " | CTRL+S save | CTRL+X exit\r\n------------------------\r\n"
msg_big:
    .asciz "nano: file too large (max 4095 bytes), not opened\r\n"
msg_noname:
    .asciz "nano: usage: nano <file>\r\n"
msg_saved:
    .asciz "\r\n[Saved!]\r\n"
msg_fail:
    .asciz "\r\n[Save failed!]\r\n"
msg_exit:
    .asciz "\r\n[Exit]\r\n"
msg_bs:
    .asciz "\b \b"

    .data
    .align 12
buffer:
    .skip 4096
len:
    .quad 0
fname:
    .skip 128
default_name:
    .asciz "untitled.txt"
