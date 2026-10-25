#include "keyboard.h"
#include "uart.h"
#include "virtio_input.h"

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
    int c = uart_try_getc();
    if (c >= 0) { *out = (char)c; return 1; }
    return 0;
}

int keyboard_has_data()
{
    char c;
    if (pending_valid) return 1;
    if (!virtio_input_try_get_key(&c)) return uart_has_data();
    pending_key = c;
    pending_valid = 1;
    return 1;
}

char keyboard_getc()
{
    char c;
    while (!keyboard_try_getc(&c)) asm volatile("yield");
    return c;
}
