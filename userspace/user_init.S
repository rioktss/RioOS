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
