#include "security.h"
#include "fs.h"
#include "uart.h"
#include "keyboard.h"
#include "string.h"

static SecurityRole current_role = SECURITY_USER;
static const char USER_PASSWORD[] = "123";
static const char ROOT_PASSWORD[] = "123";

static int security_get_char()
{
    while (1)
    {
        /* Let a foreground pixel terminal paint password prompts before blocking. */
        uart_flush_output();
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
    fs_set_credentials(1000U, 1000U);
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
    fs_set_credentials(1000U, 1000U);
    return 0;
}

int security_login_root(const char* password)
{
    if (password == 0 || !str_equal(password, ROOT_PASSWORD)) return -1;
    current_role = SECURITY_ROOT;
    fs_set_credentials(0U, 0U);
    return 0;
}

void security_logout_to_user()
{
    current_role = SECURITY_USER;
    fs_set_credentials(1000U, 1000U);
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
            return (int)length;
        }

        if (c == 8 || c == 127)
        {
            if (length > 0)
            {
                --length;
                out[length] = '\0';
                uart_puts("\b \b");
            }
            continue;
        }

        if (c >= 32 && c <= 126 && length + 1 < cap)
        {
            out[length++] = (char)c;
            out[length] = '\0';
            /* Do not echo the actual password. */
            uart_putc('*');
        }
    }
}
