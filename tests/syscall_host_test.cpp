#include "syscall.h"
#include <stdio.h>
#include <string.h>

/* ---- stubs for everything syscall.cpp links against ---- */
static char out[8192]; static unsigned out_len = 0;
void uart_putc(char c) { if (out_len < sizeof(out)) out[out_len++] = c; }
void uart_puts(const char* s) { while (*s) uart_putc(*s++); }
static const char* rx = ""; static unsigned rx_pos = 0;
int uart_try_getc() { return rx[rx_pos] ? (unsigned char)rx[rx_pos++] : -1; }
uint64_t timer_seconds() { return 7; }
uint64_t timer_millis() { return 7123; }
static uint64_t slept = 0;
void timer_delay_ms(uint64_t ms) { slept += ms; }
uint64_t heap_free() { return 4242; }
uint64_t scheduler_ticks() { return 99; }
int scheduler_current_pid() { return 1; }
int user_current_pid() { return 5; }
static int exit_code = -1;
void user_set_exit_code(int c) { exit_code = c; }
extern "C" void user_exit_return() {}

/* Fake user window: [0x47000000,0x47001000) read-only, [0x47001000,0x47002000) RW. */
int mmu_user_pointer_ok(uint64_t a, uint64_t len, int write)
{
    if (a == 0 || len == 0) return 0;
    uint64_t end = a + len;
    if (end < a) return 0;
    if (a >= 0x47000000ULL && end <= 0x47001000ULL) return !write;
    if (a >= 0x47001000ULL && end <= 0x47002000ULL) return 1;
    return 0;
}

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (0)

static uint64_t call(uint64_t nr, uint64_t a0 = 0, uint64_t a1 = 0, int user = 1)
{
    ExceptionFrame f; memset(&f, 0, sizeof f);
    f.x[8] = nr; f.x[0] = a0; f.x[1] = a1;
    syscall_dispatch(&f, user);
    return f.x[0];
}
#define ERR(n) ((uint64_t)(-(n)))

int main()
{
    CHECK(call(SYS_PING) == 0x4D594B56ULL);
    CHECK(call(SYS_GETPID, 0, 0, 1) == 5 && call(SYS_GETPID, 0, 0, 0) == 1);
    CHECK(call(99) == ERR(38));

    /* write(buf,len): EL0 pointers are validated, kernel pointers are not */
    char kbuf[8] = "abc";
    CHECK(call(SYS_WRITE_BUF, 0x1000, 4) == ERR(14));          /* kernel VA from EL0 */
    CHECK(call(SYS_WRITE_BUF, 0, 4) == ERR(14));
    CHECK(call(SYS_WRITE_BUF, 0x47000FFE, 8) == ERR(14));      /* crosses window end */
    CHECK(call(SYS_WRITE_BUF, 0x47000000, SYSCALL_IO_MAX + 1) == ERR(22));
    CHECK(call(SYS_WRITE_BUF, 0x47000000, 0) == 0);
    out_len = 0;
    CHECK(call(SYS_WRITE_BUF, (uint64_t)kbuf, 3, 0) == 3 && out_len == 3 && memcmp(out, "abc", 3) == 0);

    /* uname must demand WRITE permission on the destination */
    CHECK(call(SYS_UNAME, 0x47000000) == ERR(14));             /* read-only page */
    CHECK(call(SYS_UNAME, 0x47000FF0) == ERR(14));
    CHECK(call(SYS_UNAME, 0) == ERR(14));
    char kname[40];
    CHECK(call(SYS_UNAME, (uint64_t)kname, 0, 0) > 0 && strncmp(kname, "MyKernel", 8) == 0);

    /* read: needs write permission, non-blocking, bounded */
    char rbuf[16];
    rx = "hi"; rx_pos = 0;
    CHECK(call(SYS_READ, 0x47000000, 4) == ERR(14));           /* read-only dest */
    CHECK(call(SYS_READ, (uint64_t)rbuf, 16, 0) == 2 && memcmp(rbuf, "hi", 2) == 0);
    CHECK(call(SYS_READ, (uint64_t)rbuf, 16, 0) == 0);         /* nothing pending */
    CHECK(call(SYS_READ, (uint64_t)rbuf, SYSCALL_IO_MAX + 1, 0) == ERR(22));

    CHECK(call(SYS_SLEEP_MS, 50) == 0 && slept == 50);
    CHECK(call(SYS_SLEEP_MS, SYSCALL_SLEEP_MAX_MS + 1) == ERR(22) && slept == 50);
    CHECK(call(SYS_TIME_MS) == 7123);

    /* SVC immediate != 0 is rejected */
    ExceptionFrame f; memset(&f, 0, sizeof f); f.esr = 5; f.x[8] = SYS_PING;
    syscall_dispatch(&f, 1);
    CHECK(f.x[0] == ERR(22));

    /* exit redirects the frame and records the code */
    memset(&f, 0, sizeof f); f.x[8] = SYS_EXIT; f.x[0] = 3;
    syscall_dispatch(&f, 1);
    CHECK(exit_code == 3 && f.spsr == 0x3C5ULL && f.elr != 0);
    CHECK(call(SYS_EXIT, 0, 0, 0) == ERR(1));                  /* EL1 may not exit */

    printf("SYSCALL HOST TEST: PASS\n");
    return 0;
}
