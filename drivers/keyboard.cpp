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