#include "env.h"
#include "uart.h"
#include "string.h"

#define ENV_MAX 24
#define ENV_NAME_MAX 32
#define ENV_VALUE_MAX 128

struct EnvEntry
{
    int used;
    char name[ENV_NAME_MAX];
    char value[ENV_VALUE_MAX];
};

static EnvEntry envs[ENV_MAX];

static int valid_name_char(char c, int first)
{
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_') return 1;
    return !first && c >= '0' && c <= '9';
}

static int find_entry(const char* name)
{
    if (!name || !name[0]) return -1;
    for (int i = 0; i < ENV_MAX; ++i)
        if (envs[i].used && str_equal(envs[i].name, name)) return i;
    return -1;
}

int env_set(const char* assignment)
{
    if (!assignment) return -1;
    int eq = -1;
    for (int i = 0; assignment[i]; ++i) if (assignment[i] == '=') { eq = i; break; }
    if (eq <= 0 || eq >= ENV_NAME_MAX) return -1;
    for (int i = 0; i < eq; ++i)
        if (!valid_name_char(assignment[i], i == 0)) return -1;

    char name[ENV_NAME_MAX];
    for (int i = 0; i < eq; ++i) name[i] = assignment[i];
    name[eq] = '\0';

    int idx = find_entry(name);
    if (idx < 0)
    {
        for (int i = 0; i < ENV_MAX; ++i)
            if (!envs[i].used) { idx = i; break; }
    }
    if (idx < 0) return -2;

    envs[idx].used = 1;
    str_copy(envs[idx].name, name, ENV_NAME_MAX);
    str_copy(envs[idx].value, assignment + eq + 1, ENV_VALUE_MAX);
    return 0;
}

const char* env_get(const char* name)
{
    int idx = find_entry(name);
    return idx >= 0 ? envs[idx].value : 0;
}

void env_print()
{
    for (int i = 0; i < ENV_MAX; ++i)
    {
        if (!envs[i].used) continue;
        uart_puts(envs[i].name);
        uart_putc('=');
        uart_puts(envs[i].value);
        uart_puts("\r\n");
    }
}
