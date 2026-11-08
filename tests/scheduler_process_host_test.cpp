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

#include "process.h"
#include "scheduler.h"
#include <stdio.h>

void uart_putc(char) {}
void uart_puts(const char*) {}

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (0)

int main()
{
    process_init();
    scheduler_init();
    CHECK(scheduler_current_pid() == 0);

    int pid = process_create("logical-current");
    CHECK(pid >= 0);
    CHECK(scheduler_add(pid) == 0);
    CHECK(scheduler_set_current_logical(pid) == 0);
    CHECK(scheduler_current_pid() == pid);
    CHECK(process_get_state(pid) == PROCESS_RUNNING);
    CHECK(process_get_state(0) == PROCESS_READY);

    /* Removing the logical current must select a surviving runnable record. */
    CHECK(scheduler_remove(pid) == 0);
    CHECK(scheduler_current_pid() == 0);
    CHECK(process_get_state(0) == PROCESS_RUNNING);
    CHECK(process_mark_exit(pid, 17) == 0);
    CHECK(process_destroy(pid) == 0);

    /* A zombie queued but not current must be compacted without losing current. */
    int stale = process_create("stale-zombie");
    CHECK(stale >= 0);
    CHECK(scheduler_add(stale) == 0);
    CHECK(process_mark_exit(stale, 2) == 0);
    CHECK(scheduler_current_pid() == 0);
    CHECK(scheduler_ready_count() >= 1);
    CHECK(scheduler_remove(stale) == -1); /* compact already removed it */
    CHECK(process_destroy(stale) == 0);

    CHECK(scheduler_set_current_logical(9999) == -1);
    CHECK(scheduler_current_pid() == 0);
    printf("SCHEDULER PROCESS HOST TEST: PASS\n");
    return 0;
}
