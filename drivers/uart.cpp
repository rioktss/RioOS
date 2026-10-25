#include "types.h"
#include "uart.h"

#define UART_BASE 0x09000000ULL

static volatile uint32_t* const UART_DR =
    (volatile uint32_t*)(UART_BASE + 0x00);

static volatile uint32_t* const UART_FR =
    (volatile uint32_t*)(UART_BASE + 0x18);

static uart_output_hook_t output_hook = 0;
static int output_hook_mirror = 0;
static uart_output_flush_hook_t output_flush_hook = 0;

void uart_set_output_hook(uart_output_hook_t hook, int mirror_to_serial)
{
    output_hook = hook;
    output_hook_mirror = mirror_to_serial != 0;
}

void uart_clear_output_hook()
{
    output_hook = 0;
    output_hook_mirror = 0;
    output_flush_hook = 0;
}

int uart_output_hook_active()
{
    return output_hook != 0;
}

void uart_set_output_flush_hook(uart_output_flush_hook_t hook)
{
    output_flush_hook = hook;
}

void uart_flush_output()
{
    if (output_flush_hook != 0) output_flush_hook();
}

void uart_putc(char c)
{
    if (output_hook != 0)
    {
        output_hook(c);
        if (!output_hook_mirror) return;
    }

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

int uart_try_getc()
{
    if ((*UART_FR) & (1U << 4))
        return -1;

    return (int)(*UART_DR & 0xFFU);
}

// TAMBAHAN BARU - FIX SHELL HANG
int uart_has_data()
{
    // FR bit 4 = RXFE (RX FIFO Empty). Kalau 0 = ada data
    return ((*UART_FR) & (1U << 4)) == 0;
}