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

#include "keyboard.h"
#include "uart.h"
#include "virtio_input.h"
#include "console.h"
#include "interrupt.h"
#include "usb.h"

static char pending_key = 0;
static int pending_valid = 0;

void keyboard_init()
{
    pending_key = 0;
    pending_valid = 0;
    virtio_input_init();
    uart_puts("Keyboard initialized (UART + VirtIO input when available).\r\n");
}

int keyboard_try_getc(char* out)
{
    if (!out) return 0;
    if (pending_valid)
    {
        *out = pending_key; pending_valid = 0; return 1;
    }
    if (virtio_input_try_get_key(out)) return 1;
    if (usb_try_get_key(out)) return 1;
    int c = uart_try_getc();
    if (c >= 0) { console_snap_to_bottom(); *out = (char)c; return 1; }
    console_flush(); /* idle: make sure pending output is on screen */
    return 0;
}

int keyboard_has_data()
{
    char c;
    if (pending_valid) return 1;
    if (!virtio_input_try_get_key(&c) && !usb_try_get_key(&c)) return uart_has_data();
    pending_key = c;
    pending_valid = 1;
    return 1;
}

char keyboard_getc()
{
    char c;
    while (!keyboard_try_getc(&c)) interrupt_idle();
    return c;
}
