#include "keyboard.h"
#include "uart.h"

void keyboard_init()
{
    uart_puts(
        "Keyboard initialized.\r\n"
    );
}

char keyboard_getc()
{
    return uart_getc();
}

// TAMBAHAN BARU - ANTI HANG
int keyboard_has_data()
{
    return uart_has_data();
}