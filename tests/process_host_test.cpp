#include "process.h"
#include "mmu.h"
#include <stdio.h>
#include <string.h>

static char uart_output[8192];
static unsigned uart_used;
void uart_putc(char c) { if (uart_used < sizeof(uart_output)) uart_output[uart_used++] = c; }
void uart_puts(const char* s) { if (!s) return; while (*s) uart_putc(*s++); }

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (0)

int main()
{
    process_init();
    CHECK(process_count() == 2);
    CHECK(process_pid_at(0) == 0);
    CHECK(process_pid_at(1) == 1);
    CHECK(process_get_state(0) == PROCESS_RUNNING);
    CHECK(process_get_state(1) == PROCESS_READY);

    CHECK(process_create(0) == -1);
    CHECK(process_create("") == -1);
    int pid = process_create("p0-test");
    CHECK(pid == 2);
    CHECK(process_get_state(pid) == PROCESS_READY);

    /* A process can be configured only in the reserved EL0 virtual window. */
    CHECK(process_configure_user(pid, 0x1000, MMU_USER_STACK_TOP) == -1);
    CHECK(process_configure_user(pid, MMU_USER_BASE, MMU_USER_STACK_TOP + MMU_PAGE_SIZE) == -1);
    CHECK(process_configure_user(pid, MMU_USER_BASE + 1, MMU_USER_LIMIT) == -1);
    CHECK(process_configure_user(pid, MMU_USER_BASE, MMU_USER_STACK_TOP) == 0);
    CHECK(process_is_user(pid) == 1);
    CHECK(process_user_low(pid) == MMU_USER_BASE);
    CHECK(process_user_high(pid) == MMU_USER_STACK_TOP);

    /* P5: the PCB retains a complete EL0 register frame at an exception
       boundary and can provide it to a future process-frame scheduler. */
    ProcessCpuContext saved;
    ProcessCpuContext restored;
    for (int r = 0; r < 31; ++r) saved.x[r] = 0xA500ULL + (uint64_t)r;
    saved.elr = 0x47001234ULL;
    saved.spsr = 0;
    saved.user_sp = MMU_USER_STACK_TOP - 16ULL;
    CHECK(process_context_valid(pid) == 0);
    CHECK(process_save_context(pid, &saved) == 0);
    CHECK(process_context_valid(pid) == 1);
    CHECK(process_load_context(pid, &restored) == 0);
    for (int r = 0; r < 31; ++r) CHECK(restored.x[r] == saved.x[r]);
    CHECK(restored.elr == saved.elr);
    CHECK(restored.spsr == saved.spsr);
    CHECK(restored.user_sp == saved.user_sp);

    /* Only one process-table entry may be RUNNING on a uniprocessor. */
    CHECK(process_set_state(pid, PROCESS_RUNNING) == 0);
    CHECK(process_get_state(pid) == PROCESS_RUNNING);
    CHECK(process_get_state(0) == PROCESS_READY);
    CHECK(process_set_state(0, PROCESS_RUNNING) == 0);
    CHECK(process_get_state(0) == PROCESS_RUNNING);
    CHECK(process_get_state(pid) == PROCESS_READY);

    CHECK(process_set_state(pid, PROCESS_UNUSED) == -1);
    CHECK(process_set_state(pid, 123) == -1);
    CHECK(process_set_state(999, PROCESS_READY) == -1);
    CHECK(process_destroy(0) == -2);
    CHECK(process_destroy(1) == -2);

    CHECK(process_set_state(pid, PROCESS_RUNNING) == 0);
    CHECK(process_destroy(pid) == -3); /* refuse to destroy RUNNING */
    CHECK(process_mark_exit(pid, 42) == 0);
    CHECK(process_get_state(pid) == PROCESS_ZOMBIE);
    CHECK(process_exit_code(pid) == 42);
    CHECK(process_set_state(pid, PROCESS_READY) == -1); /* zombie stays terminal */
    CHECK(process_mark_exit(0, 1) == -2);
    CHECK(process_mark_exit(1, 1) == -2);
    CHECK(process_destroy(pid) == 0);
    CHECK(process_context_valid(pid) == 0);
    CHECK(process_destroy(pid) == -1);
    CHECK(process_count() == 2);

    printf("PROCESS HOST TEST: PASS\n");
    return 0;
}
