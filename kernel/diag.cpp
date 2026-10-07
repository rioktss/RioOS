#include "diag.h"
#include "uart.h"

#ifdef HOST_TEST
#include <stdlib.h>
#endif

static int level_threshold = LOG_WARN;
static uint64_t printed = 0;
static uint64_t suppressed = 0;

void diag_set_level(int level)
{
    if (level < LOG_ERROR)
        level = LOG_ERROR;
    if (level > LOG_DEBUG)
        level = LOG_DEBUG;

    level_threshold = level;
}

int diag_get_level()
{
    return level_threshold;
}

const char* diag_level_name(int level)
{
    switch (level)
    {
        case LOG_ERROR: return "ERROR";
        case LOG_WARN:  return "WARN";
        case LOG_INFO:  return "INFO";
        case LOG_DEBUG: return "DEBUG";
        default:        return "?";
    }
}

void klog(int level, const char* message)
{
    if (message == 0)
        return;

    if (level > level_threshold)
    {
        ++suppressed;
        return;
    }

    ++printed;
    uart_puts("[");
    uart_puts(diag_level_name(level));
    uart_puts("] ");
    uart_puts(message);
    uart_puts("\r\n");
}

uint64_t diag_log_printed() { return printed; }
uint64_t diag_log_suppressed() { return suppressed; }

static void put_dec(int value)
{
    char buf[12];
    int n = 0;

    if (value < 0)
        value = 0;
    if (value == 0)
        buf[n++] = '0';

    while (value > 0 && n < 11)
    {
        buf[n++] = (char)('0' + (value % 10));
        value /= 10;
    }

    while (n > 0)
        uart_putc(buf[--n]);
}

void kpanic(const char* message, const char* file, int line)
{
#ifndef HOST_TEST
    asm volatile("msr daifset, #0xf" ::: "memory");
#endif

    uart_puts("\r\n*** KERNEL PANIC ***\r\n");
    uart_puts(message != 0 ? message : "(no message)");
    uart_puts("\r\n at ");
    uart_puts(file != 0 ? file : "?");
    uart_puts(":");
    put_dec(line);
    uart_puts("\r\nSystem halted.\r\n");

#ifdef HOST_TEST
    abort();
#else
    while (1)
        asm volatile("wfe");
#endif
}
