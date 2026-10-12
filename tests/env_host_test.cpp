#include "env.h"
#include <stdio.h>
#include <string.h>

void uart_putc(char) {}
void uart_puts(const char*) {}

#define CHECK(c) do { if (!(c)) { printf("ENV HOST TEST FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (0)

int main()
{
    CHECK(env_set("RIOOS_MODE=dev") == 0);
    CHECK(env_get("RIOOS_MODE") != 0);
    CHECK(strcmp(env_get("RIOOS_MODE"), "dev") == 0);
    CHECK(env_set("RIOOS_MODE=release") == 0);
    CHECK(strcmp(env_get("RIOOS_MODE"), "release") == 0);
    CHECK(env_set("9BAD=value") == -1);
    CHECK(env_set("=empty") == -1);
    CHECK(env_unset("RIOOS_MODE") == 0);
    CHECK(env_get("RIOOS_MODE") == 0);
    CHECK(env_unset("RIOOS_MODE") == -2);
    CHECK(env_unset("bad-name") == -1);
    puts("ENV HOST TEST: PASS");
    return 0;
}
