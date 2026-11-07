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

/* Error probe: a built-in command that fails prints "<name>: ..." (or "Unknown command:") at the
   start of a line. The shell uses this to give built-ins a real exit status. */
static const char* probe_name = 0;
static int probe_len = 0;
static int probe_pos = -1;
static int probe_line_start = 1;
static int probe_hit = 0;

void uart_error_probe_begin(const char* name, int len, UartProbeState* save)
{
    if (save) { save->name = probe_name; save->len = probe_len; save->hit = probe_hit; }
    probe_name = name; probe_len = len; probe_pos = -1; probe_hit = 0; probe_line_start = 1;
}

int uart_error_probe_end(const UartProbeState* restore)
{
    int hit = probe_hit;
    if (restore) { probe_name = restore->name; probe_len = restore->len; probe_hit = restore->hit; }
    else { probe_name = 0; probe_len = 0; probe_hit = 0; }
    probe_pos = -1;
    return hit;
}

static void probe_feed(char c)
{
    if (!probe_name) return;
    static const char unk[] = "Unknown command:";
    static int unk_pos = -1;
    if (probe_line_start) { probe_pos = 0; unk_pos = 0; }
    if (probe_pos >= 0)
    {
        if (probe_pos < probe_len) { if (c == probe_name[probe_pos]) ++probe_pos; else probe_pos = -1; }
        else { if (c == ':' && probe_len > 0) probe_hit = probe_hit ? probe_hit : 1; probe_pos = -1; }
    }
    if (unk_pos >= 0)
    {
        if (unk[unk_pos] == '\0') { probe_hit = 2; unk_pos = -1; }
        else if (c == unk[unk_pos]) { ++unk_pos; if (unk[unk_pos] == '\0') { probe_hit = 2; unk_pos = -1; } }
        else unk_pos = -1;
    }
    probe_line_start = (c == '\n');
}

int uart_at_line_start(void) { return probe_line_start; }

void uart_putc(char c)
{
    probe_feed(c);
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

/* Bytes pulled off the UART by the job-control poller but not consumed by it. */
static uint8_t rx_ring[256];
static unsigned rx_head = 0, rx_tail = 0;

int uart_try_getc()
{
    if (rx_head != rx_tail)
    {
        int c = rx_ring[rx_tail];
        rx_tail = (rx_tail + 1U) & 255U;
        return c;
    }
    if ((*UART_FR) & (1U << 4))
        return -1;

    return (int)(*UART_DR & 0xFFU);
}

/* Drain the hardware FIFO: bytes for which filter() returns non-zero (Ctrl-C /
   Ctrl-Z while a foreground job runs) are consumed, everything else is queued
   in order for the next uart_try_getc(). */
void uart_poll_job_keys(int (*filter)(int))
{
    while (((*UART_FR) & (1U << 4)) == 0)
    {
        int c = (int)(*UART_DR & 0xFFU);
        if (filter && filter(c)) continue;
        unsigned next = (rx_head + 1U) & 255U;
        if (next == rx_tail) break;              /* ring full: stop draining */
        rx_ring[rx_head] = (uint8_t)c;
        rx_head = next;
    }
}

// TAMBAHAN BARU - FIX SHELL HANG
int uart_has_data()
{
    // FR bit 4 = RXFE (RX FIFO Empty). Kalau 0 = ada data
    return rx_head != rx_tail || ((*UART_FR) & (1U << 4)) == 0;
}