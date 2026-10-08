#include "history.h"
#include "uart.h"
#include "string.h"

#define HISTORY_MAX 32
#define HISTORY_LINE_MAX 256

static char entries[HISTORY_MAX][HISTORY_LINE_MAX];
static int count = 0;
static int next_slot = 0;

void history_init()
{
    count = 0;
    next_slot = 0;
    for (int i = 0; i < HISTORY_MAX; ++i)
        entries[i][0] = '\0';
}

void history_add(const char* line)
{
    if (line == 0 || line[0] == '\0') return;
    if (count > 0)
    {
        int last = (next_slot - 1 + HISTORY_MAX) % HISTORY_MAX;
        if (str_equal(entries[last], line)) return;
    }
    str_copy(entries[next_slot], line, HISTORY_LINE_MAX);
    next_slot = (next_slot + 1) % HISTORY_MAX;
    if (count < HISTORY_MAX) ++count;
}

int history_count()
{
    return count;
}

const char* history_get(int index)
{
    if (index < 0 || index >= count) return 0;
    int oldest = (next_slot - count + HISTORY_MAX) % HISTORY_MAX;
    int slot = (oldest + index) % HISTORY_MAX;
    return entries[slot];
}

void history_clear()
{
    count = 0;
    next_slot = 0;
    for (int i = 0; i < HISTORY_MAX; ++i)
        entries[i][0] = '\0';
}

static void print_index(int index, const char* text)
{
    char digits[12];
    int n = 0;
    int value = index;
    do { digits[n++] = (char)('0' + (value % 10)); value /= 10; } while (value && n < (int)sizeof(digits));
    while (n > 0) uart_putc(digits[--n]);
    uart_puts("  ");
    uart_puts(text);
    uart_puts("\r\n");
}

void history_print(int last_n)
{
    if (count == 0) return;
    if (last_n <= 0 || last_n > count) last_n = count;
    int start = count - last_n;
    for (int i = start; i < count; ++i)
        print_index(i + 1, history_get(i));
}
