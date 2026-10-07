#include "shell.h"
#include "keyboard.h"
#include "commands.h"
#include "uart.h"

void shell()
{
    char input[256];

    while (1)
    {
        uart_puts(
            "[home@RioOS ]$ "
        );

        int length = 0;

        while (1)
        {
            char c =
                keyboard_getc();

            // Enter
            if (c == '\r' ||
                c == '\n')
            {
                input[length] =
                    '\0';

                uart_puts(
                    "\r\n"
                );

                break;
            }

            // Backspace
            if (c == 8 ||
                c == 127)
            {
                if (length > 0)
                {
                    length--;

                    input[length] =
                        '\0';

                    uart_puts(
                        "\b \b"
                    );
                }

                continue;
            }

            // Printable ASCII
            if (c >= 32 &&
                c <= 126)
            {
                if (length < 255)
                {
                    input[length++] =
                        c;

                    input[length] =
                        '\0';

                    uart_putc(c);
                }

                continue;
            }
        }

        execute_command(
            input
        );

        uart_puts(
            "\r\n"
        );
    }
}