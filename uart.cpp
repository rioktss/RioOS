#include "types.h"
#include "uart.h"

#define UART_BASE 0x09000000ULL

static volatile uint32_t* const UART_DR =
    (volatile uint32_t*)(UART_BASE + 0x00);

static volatile uint32_t* const UART_FR =
    (volatile uint32_t*)(UART_BASE + 0x18);

void uart_putc(char c)
{
    while ((*UART_FR) & (1U << 5))
    {
    }

    *UART_DR = (uint32_t)c;
}

void uart_puts(const char* s)
{
    if (s == 0)
        return;

    while (*s)
    {
        uart_putc(*s);
        s++;
    }
}

char uart_getc()
{
    while ((*UART_FR) & (1U << 4))
    {
    }

    return (char)(*UART_DR & 0xFF);
}