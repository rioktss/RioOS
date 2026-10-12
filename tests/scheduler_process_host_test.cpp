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
