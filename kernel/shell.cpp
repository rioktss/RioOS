#include "shell.h"
#include "keyboard.h"
#include "commands.h"
#include "uart.h"

// opsional kalau belum ada fb, gak error
__attribute__((weak)) void fb_putc(char c, unsigned int color) { (void)c; (void)color; }
__attribute__((weak)) void fb_print(const char* s, unsigned int color) { (void)s; (void)color; }

void shell()
{
    char input[256];

    while (1)
    {
        uart_puts(
            "[home@RioOS]$ "
        );
        if(fb_print) fb_print("[home@RioOS]$ ", 0x00FF00);

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
                if(fb_print) fb_print("\r\n", 0xFFFFFF);

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
                    if(fb_putc){
                        fb_putc('\b', 0xFFFFFF);
                    }
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
                    if(fb_putc) fb_putc(c, 0xFFFFFF);
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
        if(fb_print) fb_print("\r\n", 0xFFFFFF);
    }
}