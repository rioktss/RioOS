#include "security.h"
#include "uart.h"
#include "keyboard.h"
#include "fb.h"
#include "string.h"

static SecurityRole current_role = SECURITY_USER;
static const char USER_PASSWORD[] = "123";

static int security_get_char()
{
    while (1)
    {
        if (uart_has_data())
            return (int)(unsigned char)uart_getc();

        if (keyboard_has_data())
            return (int)(unsigned char)keyboard_getc();

        asm volatile("yield");
    }
}

void security_init()
{
    current_role = SECURITY_USER;
}

SecurityRole security_role()
{
    return current_role;
}

int security_is_root()
{
    return current_role == SECURITY_ROOT;
}

const char* security_username()
{
    return current_role == SECURITY_ROOT ? "root" : "user";
}

char security_prompt_char()
{
    return current_role == SECURITY_ROOT ? '#' : '$';
}

int security_login_user(const char* password)
{
    if (password == 0 || !str_equal(password, USER_PASSWORD))
        return -1;

    current_role = SECURITY_USER;
    return 0;
}

void security_login_root()
{
    current_role = SECURITY_ROOT;
}

void security_logout_to_user()
{
    current_role = SECURITY_USER;
}

int security_read_password(char* out, uint64_t cap)
{
    if (out == 0 || cap < 2)
        return -1;

    uint64_t length = 0;
    out[0] = '\0';

    while (1)
    {
        int c = security_get_char();

        if (c == '\r' || c == '\n')
        {
            out[length] = '\0';
            uart_puts("\r\n");
            fb_print("\r\n", 0xFFFFFF);
            return (int)length;
        }

        if (c == 8 || c == 127)
        {
            if (length > 0)
            {
                --length;
                out[length] = '\0';
                uart_puts("\b \b");
                fb_putc('\b', 0xFFFFFF);
            }
            continue;
        }

        if (c >= 32 && c <= 126 && length + 1 < cap)
        {
            out[length++] = (char)c;
            out[length] = '\0';
            /* Do not echo the actual password. */
            uart_putc('*');
            fb_putc('*', 0xFFFFFF);
        }
    }
}
