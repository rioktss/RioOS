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

#ifndef UART_H
#define UART_H

typedef void (*uart_output_hook_t)(char value);
typedef void (*uart_output_flush_hook_t)();

void uart_init();
void uart_putc(char c);
void uart_puts(const char* s);
int uart_at_line_start(void);   /* 1 when the last character written was a newline */
/* Serial only: not mirrored to the framebuffer console. */
void uart_putc_serial_only(char c);
void uart_puts_serial_only(const char* s);
/* Non-blocking serial input: returns the next byte (0..255) or -1 when none is pending. */
int uart_try_getc();

struct UartProbeState { const char* name; int len; int hit; };
void uart_error_probe_begin(const char* name, int len, UartProbeState* save);
int uart_error_probe_end(const UartProbeState* restore);   /* 0 none, 1 "<name>: error", 2 "Unknown command" */
void uart_poll_job_keys(int (*filter)(int));
int uart_has_data();
char uart_getc();
/* Temporarily route output to a foreground console (for example the pixel terminal). */
void uart_set_output_hook(uart_output_hook_t hook, int mirror_to_serial);
void uart_clear_output_hook();
int uart_output_hook_active();
void uart_set_output_flush_hook(uart_output_flush_hook_t hook);
void uart_flush_output();

#endif
