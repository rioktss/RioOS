#ifndef UART_H
#define UART_H

typedef void (*uart_output_hook_t)(char value);
typedef void (*uart_output_flush_hook_t)();

void uart_putc(char c);
void uart_puts(const char* s);
/* Non-blocking serial input: returns the next byte (0..255) or -1 when none is pending. */
int uart_try_getc();
int uart_has_data();
char uart_getc();
/* Temporarily route output to a foreground console (for example the pixel terminal). */
void uart_set_output_hook(uart_output_hook_t hook, int mirror_to_serial);
void uart_clear_output_hook();
int uart_output_hook_active();
void uart_set_output_flush_hook(uart_output_flush_hook_t hook);
void uart_flush_output();

#endif
