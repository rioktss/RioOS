#include "types.h"
#include "uart.h"
#include "console.h"

#define UART_BASE 0x09000000ULL

static volatile uint32_t* const UART_DR =
    (volatile uint32_t*)(UART_BASE + 0x00);

static volatile uint32_t* const UART_FR =
    (volatile uint32_t*)(UART_BASE + 0x18);

void uart_init()
{
    /* PL011: QEMU only accepts RX bytes once UARTEN is set (reset value 0x300
       leaves it clear). Program 8N1 + FIFO, mask UART IRQs (polled), enable. */
    volatile uint32_t* const cr = (volatile uint32_t*)(UART_BASE + 0x30);
    volatile uint32_t* const lcrh = (volatile uint32_t*)(UART_BASE + 0x2C);
    volatile uint32_t* const imsc = (volatile uint32_t*)(UART_BASE + 0x38);
    volatile uint32_t* const icr = (volatile uint32_t*)(UART_BASE + 0x44);
    *cr = 0;
    *lcrh = 0x70U;          /* 8 bits, FIFO enabled, no parity */
    *imsc = 0;
    *icr = 0x7FFU;
    *cr = 0x301U;           /* UARTEN | TXE | RXE */
    asm volatile("dsb sy" ::: "memory");
}

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
    console_flush();
}

void uart_putc_serial_only(char c)
{
    while ((*UART_FR) & (1U << 5))
    {
    }

    *UART_DR = (uint32_t)c;
}

void uart_puts_serial_only(const char* s)
{
    if (s == 0)
        return;

    while (*s)
        uart_putc_serial_only(*s++);
}

void uart_putc(char c)
{
    if (output_hook != 0)
    {
        output_hook(c);
        if (!output_hook_mirror) return;
    }

    /* Everything printed to the serial console is mirrored to the QEMU
       window terminal once it is active. */
    if (console_active()) console_putc(c);

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