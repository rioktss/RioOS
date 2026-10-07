#ifndef UART_H
#define UART_H

void uart_putc(char c);
void uart_puts(const char* s);
/* Non-blocking: returns the next byte (0..255) or -1 when none is pending. */
int uart_try_getc();
int uart_has_data();
char uart_getc();

#endif