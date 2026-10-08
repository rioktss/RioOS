#include "shell.h"
#include "keyboard.h"
#include "commands.h"
#include "uart.h"
#include "fb.h"
#include "vfs.h"

void shell()
{
    char input[256];

    while (1)
    {
        char cwd[96];
        vfs_get_path(cwd, (int)sizeof(cwd));

        uart_puts("[home@RioOS ");
        uart_puts(cwd);
        uart_puts("]$ ");
        fb_print("[home@RioOS ", 0x00FF00);
        fb_print(cwd, 0x00FF00);
        fb_print("]$ ", 0x00FF00);

        int length = 0;

        while (1)
        {
            char c = 0;

            // FIX UTAMA: jangan cuma keyboard_getc, coba uart_getc dulu (anti-hang QEMU virt)
            // kalau IRQ mati, keyboard_getc bakal hang. uart_getc polling selalu jalan
            if(uart_has_data()){
                c = uart_getc();
            } else {
                // kalau ada data keyboard (PS/2), pakai itu
                // tapi jangan blocking selamanya
                if(keyboard_has_data()){
                    c = keyboard_getc();
                } else {
                    // polling loop, kasih hint ke CPU biar gak panas
                    asm volatile("yield");
                    continue;
                }
            }

            // Enter
            if (c == '\r' ||
                c == '\n')
            {
                input[length] =
                    '\0';

                uart_puts(
                    "\r\n"
                );
                fb_print("\r\n", 0xFFFFFF);

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
                    fb_putc('\b', 0xFFFFFF);
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
                    fb_putc(c, 0xFFFFFF);
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
        fb_print("\r\n", 0xFFFFFF);
    }
}