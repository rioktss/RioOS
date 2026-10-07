#include "diag.h"
#include <stdio.h>
#include <string.h>

static char out[1024]; static unsigned n = 0;
void uart_putc(char c) { if (n < sizeof(out) - 1) { out[n++] = c; out[n] = 0; } }
void uart_puts(const char* s) { while (*s) uart_putc(*s++); }

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (0)

int main()
{
    CHECK(diag_get_level() == LOG_WARN);

    klog(LOG_INFO, "hidden");
    CHECK(n == 0 && diag_log_suppressed() == 1 && diag_log_printed() == 0);

    klog(LOG_ERROR, "boom");
    CHECK(strcmp(out, "[ERROR] boom\r\n") == 0 && diag_log_printed() == 1);

    diag_set_level(LOG_DEBUG);
    n = 0; out[0] = 0;
    klog(LOG_DEBUG, "dbg");
    CHECK(strcmp(out, "[DEBUG] dbg\r\n") == 0);

    diag_set_level(99);  CHECK(diag_get_level() == LOG_DEBUG);
    diag_set_level(-5);  CHECK(diag_get_level() == LOG_ERROR);

    klog(LOG_ERROR, 0);  /* null message ignored */
    printf("DIAG HOST TEST: PASS\n");
    return 0;
}
