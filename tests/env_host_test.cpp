/*
 * MIT License
 *
 * Copyright (c) 2026 Rio okta kusuma
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files, to deal in the Software
 * without restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies of the
 * Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

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
