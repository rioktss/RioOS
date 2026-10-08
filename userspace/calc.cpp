#include "user_api.h"
#include "types.h"

/*
 * RioOS userspace calculator.
 *
 * Features:
 *   - +  -  *  /  %
 *   - ^ (power)
 *   - &  |  ^  ~  <<  >> (64-bit programmer operations)
 *   - parentheses and unary +/-
 *   - ans variable
 *   - sqrt(n), abs(n), fact(n), pow(a,b), min(a,b), max(a,b)
 *   - memory: m+, m-, mr, mc
 *   - history of recent expressions
 *
 * No timer/scheduler syscall is used. Input is polled through SYS_READ,
 * so the application works with timer IRQ kept disabled.
 */

static const int64_t I64_MIN = (-9223372036854775807LL - 1LL);
static const int64_t I64_MAX =  9223372036854775807LL;

static const int INPUT_MAX = 192;
static const int HISTORY_MAX = 8;
static const int HISTORY_LEN = 128;

static int64_t g_ans = 0;
static int64_t g_mem = 0;

static char g_history[HISTORY_MAX][HISTORY_LEN];
static int g_history_count = 0;
static int g_history_next = 0;

static void write_str(const char* s)
{
    if (!s) return;
    int n = 0;
    while (s[n] && n < 1024) ++n;
    if (n) (void)user_write(s, (uint64_t)n);
}

static void write_char(char c)
{
    (void)user_write(&c, 1);
}

static void write_u64(uint64_t value)
{
    char buf[32];
    int n = 0;
    if (value == 0) { write_char('0'); return; }
    while (value && n < (int)sizeof(buf))
    {
        buf[n++] = (char)('0' + (value % 10ULL));
        value /= 10ULL;
    }
    while (n) write_char(buf[--n]);
}

static void write_i64(int64_t value)
{
    if (value < 0)
    {
        write_char('-');
        /* Convert without overflowing on INT64_MIN. */
        uint64_t magnitude = (uint64_t)(-(value + 1LL)) + 1ULL;
        write_u64(magnitude);
        return;
    }
    write_u64((uint64_t)value);
}

static void write_hex64(uint64_t value)
{
    static const char hex[] = "0123456789ABCDEF";
    write_str("0x");
    for (int i = 15; i >= 0; --i)
        write_char(hex[(value >> (i * 4)) & 0xFULL]);
}

static int str_eq(const char* a, const char* b)
{
    if (!a || !b) return 0;
    int i = 0;
    while (a[i] && b[i] && a[i] == b[i]) ++i;
    return a[i] == 0 && b[i] == 0;
}

static int str_starts(const char* a, const char* prefix)
{
    if (!a || !prefix) return 0;
    int i = 0;
    while (prefix[i])
    {
        if (a[i] != prefix[i]) return 0;
        ++i;
    }
    return 1;
}

static void copy_str(char* dst, const char* src, int cap)
{
    if (!dst || cap <= 0) return;
    int i = 0;
    if (src)
    {
        while (src[i] && i < cap - 1)
        {
            dst[i] = src[i];
            ++i;
        }
    }
    dst[i] = 0;
}

static void add_history(const char* line)
{
    if (!line || !line[0]) return;
    copy_str(g_history[g_history_next], line, HISTORY_LEN);
    g_history_next = (g_history_next + 1) % HISTORY_MAX;
    if (g_history_count < HISTORY_MAX) ++g_history_count;
}

static void show_history()
{
    write_str("History\r\n-------\r\n");
    if (g_history_count == 0)
    {
        write_str("(empty)\r\n");
        return;
    }

    int start = (g_history_count == HISTORY_MAX) ? g_history_next : 0;
    for (int i = 0; i < g_history_count; ++i)
    {
        int index = (start + i) % HISTORY_MAX;
        write_u64((uint64_t)(i + 1));
        write_str(": ");
        write_str(g_history[index]);
        write_str("\r\n");
    }
}

static void show_help()
{
    write_str(
        "\033[1;36mRioOS Calculator\033[0m\r\n"
        "================\r\n"
        "Expressions:\r\n"
        "  + - * / %  ** (power)  ^ (xor)\r\n"
        "  & |  << >>  ~\r\n"
        "  (parentheses) and unary + / -\r\n"
        "  Decimal, 0xHEX and 0bBINARY integers\r\n"
        "Functions:\r\n"
        "  sqrt(n)  abs(n)  fact(n)  pow(a,b)\r\n"
        "  min(a,b) max(a,b)\r\n"
        "Variables / commands:\r\n"
        "  ans  m+  m-  mr  mc  history\r\n"
        "  help  clear  exit / quit\r\n"
        "Examples:\r\n"
        "  2 + 3 * 4\r\n"
        "  (10 - 3) * 8\r\n"
        "  0xFF & 0x0F\r\n"
        "  sqrt(144) + fact(5)\r\n"
        "  pow(2,10)\r\n"
    );
}

struct Parser
{
    const char* s;
    int pos;
    int error;

    enum Error
    {
        ERR_NONE = 0,
        ERR_SYNTAX,
        ERR_NUMBER,
        ERR_DIV_ZERO,
        ERR_OVERFLOW,
        ERR_SHIFT,
        ERR_DOMAIN,
        ERR_UNKNOWN_NAME
    };

    void ws()
    {
        while (s[pos] == ' ' || s[pos] == '\t') ++pos;
    }

    int take(char c)
    {
        ws();
        if (s[pos] == c) { ++pos; return 1; }
        return 0;
    }

    int identifier(const char* name)
    {
        ws();
        int i = 0;
        while (name[i])
        {
            if (s[pos + i] != name[i]) return 0;
            ++i;
        }
        char c = s[pos + i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')
            return 0;
        pos += i;
        return 1;
    }

    int64_t parse_number()
    {
        ws();
        const int start = pos;
        int base = 10;
        if (s[pos] == '0' && (s[pos + 1] == 'x' || s[pos + 1] == 'X'))
        {
            base = 16;
            pos += 2;
        }
        else if (s[pos] == '0' && (s[pos + 1] == 'b' || s[pos + 1] == 'B'))
        {
            base = 2;
            pos += 2;
        }

        int digits = 0;
        uint64_t value = 0;
        const uint64_t max_positive = (uint64_t)I64_MAX;

        while (1)
        {
            int digit = -1;
            char c = s[pos];
            if (c >= '0' && c <= '9') digit = c - '0';
            else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;

            if (digit < 0 || digit >= base) break;
            ++digits;
            if (value > (max_positive - (uint64_t)digit) / (uint64_t)base)
            {
                error = ERR_NUMBER;
                return 0;
            }
            value = value * (uint64_t)base + (uint64_t)digit;
            ++pos;
        }

        if (digits == 0)
        {
            pos = start;
            error = ERR_SYNTAX;
            return 0;
        }
        return (int64_t)value;
    }

    int64_t checked_add(int64_t a, int64_t b)
    {
        int64_t out;
        if (__builtin_add_overflow(a, b, &out)) { error = ERR_OVERFLOW; return 0; }
        return out;
    }

    int64_t checked_sub(int64_t a, int64_t b)
    {
        int64_t out;
        if (__builtin_sub_overflow(a, b, &out)) { error = ERR_OVERFLOW; return 0; }
        return out;
    }

    int64_t checked_mul(int64_t a, int64_t b)
    {
        int64_t out;
        if (__builtin_mul_overflow(a, b, &out)) { error = ERR_OVERFLOW; return 0; }
        return out;
    }

    int64_t checked_div(int64_t a, int64_t b)
    {
        if (b == 0) { error = ERR_DIV_ZERO; return 0; }
        if (a == I64_MIN && b == -1LL) { error = ERR_OVERFLOW; return 0; }
        return a / b;
    }

    int64_t parse_primary();
    int64_t parse_unary();
    int64_t parse_power();
    int64_t parse_mul();
    int64_t parse_add();
    int64_t parse_shift();
    int64_t parse_and();
    int64_t parse_xor();
    int64_t parse_or();
    int64_t parse_expr();
};

static int64_t integer_sqrt(int64_t value, int* error)
{
    if (value < 0) { *error = Parser::ERR_DOMAIN; return 0; }
    uint64_t n = (uint64_t)value;
    uint64_t lo = 0, hi = 3037000500ULL; /* floor(sqrt(INT64_MAX)) */
    uint64_t ans = 0;
    while (lo <= hi)
    {
        uint64_t mid = lo + (hi - lo) / 2ULL;
        if (mid == 0 || mid <= n / mid)
        {
            ans = mid;
            lo = mid + 1ULL;
        }
        else
        {
            hi = mid - 1ULL;
        }
    }
    return (int64_t)ans;
}

static int64_t integer_abs(int64_t value, int* error)
{
    if (value == I64_MIN) { *error = Parser::ERR_OVERFLOW; return 0; }
    return value < 0 ? -value : value;
}

static int64_t integer_fact(int64_t value, int* error)
{
    if (value < 0 || value > 20) { *error = Parser::ERR_DOMAIN; return 0; }
    int64_t out = 1;
    for (int64_t i = 2; i <= value; ++i)
    {
        int64_t next;
        if (__builtin_mul_overflow(out, i, &next)) { *error = Parser::ERR_OVERFLOW; return 0; }
        out = next;
    }
    return out;
}

int64_t Parser::parse_primary()
{
    ws();

    if (take('('))
    {
        int64_t v = parse_expr();
        if (!take(')') && error == ERR_NONE) error = ERR_SYNTAX;
        return v;
    }

    if (identifier("ans")) return g_ans;

    if (identifier("sqrt"))
    {
        if (!take('(')) { error = ERR_SYNTAX; return 0; }
        int64_t a = parse_expr();
        if (!take(')') && error == ERR_NONE) error = ERR_SYNTAX;
        if (error != ERR_NONE) return 0;
        return integer_sqrt(a, &error);
    }

    if (identifier("abs"))
    {
        if (!take('(')) { error = ERR_SYNTAX; return 0; }
        int64_t a = parse_expr();
        if (!take(')') && error == ERR_NONE) error = ERR_SYNTAX;
        if (error != ERR_NONE) return 0;
        return integer_abs(a, &error);
    }

    if (identifier("fact"))
    {
        if (!take('(')) { error = ERR_SYNTAX; return 0; }
        int64_t a = parse_expr();
        if (!take(')') && error == ERR_NONE) error = ERR_SYNTAX;
        if (error != ERR_NONE) return 0;
        return integer_fact(a, &error);
    }

    if (identifier("pow") || identifier("min") || identifier("max"))
    {
        /* Re-read the function name from the expression by looking backward
           is awkward, so this branch is handled by a compact local scanner. */
        int end = pos;
        int begin = end;
        while (begin > 0 && ((s[begin - 1] >= 'a' && s[begin - 1] <= 'z') ||
                             (s[begin - 1] >= 'A' && s[begin - 1] <= 'Z')))
            --begin;
        const char* name = s + begin;
        int nlen = end - begin;

        if (!take('(')) { error = ERR_SYNTAX; return 0; }
        int64_t a = parse_expr();
        if (!take(',')) { error = ERR_SYNTAX; return 0; }
        int64_t b = parse_expr();
        if (!take(')')) { if (error == ERR_NONE) error = ERR_SYNTAX; return 0; }
        if (error != ERR_NONE) return 0;

        if (nlen == 3 && name[0] == 'p' && name[1] == 'o' && name[2] == 'w')
        {
            if (b < 0) { error = ERR_DOMAIN; return 0; }
            int64_t result = 1;
            int64_t base = a;
            uint64_t exp = (uint64_t)b;
            while (exp)
            {
                if (exp & 1ULL) result = checked_mul(result, base);
                if (error != ERR_NONE) return 0;
                exp >>= 1;
                if (exp) base = checked_mul(base, base);
                if (error != ERR_NONE) return 0;
            }
            return result;
        }
        if (nlen == 3 && name[0] == 'm' && name[1] == 'i' && name[2] == 'n')
            return a < b ? a : b;
        if (nlen == 3 && name[0] == 'm' && name[1] == 'a' && name[2] == 'x')
            return a > b ? a : b;

        error = ERR_UNKNOWN_NAME;
        return 0;
    }

    ws();
    if ((s[pos] >= '0' && s[pos] <= '9'))
        return parse_number();

    error = ERR_UNKNOWN_NAME;
    return 0;
}

int64_t Parser::parse_unary()
{
    ws();
    if (take('+')) return parse_unary();
    if (take('-'))
    {
        int64_t v = parse_unary();
        if (v == I64_MIN) { error = ERR_OVERFLOW; return 0; }
        return -v;
    }
    if (take('~')) return (int64_t)(~(uint64_t)parse_unary());
    return parse_power();
}

int64_t Parser::parse_power()
{
    int64_t base = parse_primary();
    if (error != ERR_NONE) return 0;
    ws();
    if (s[pos] == '*' && s[pos + 1] == '*')
    {
        pos += 2;
        int64_t exponent = parse_unary();
        if (error != ERR_NONE) return 0;
        if (exponent < 0) { error = ERR_DOMAIN; return 0; }
        int64_t result = 1;
        int64_t factor = base;
        uint64_t e = (uint64_t)exponent;
        while (e)
        {
            if (e & 1ULL) result = checked_mul(result, factor);
            if (error != ERR_NONE) return 0;
            e >>= 1;
            if (e) factor = checked_mul(factor, factor);
            if (error != ERR_NONE) return 0;
        }
        return result;
    }
    return base;
}

int64_t Parser::parse_mul()
{
    int64_t left = parse_unary();
    while (error == ERR_NONE)
    {
        ws();
        char op = s[pos];
        if (op != '*' && op != '/' && op != '%') break;
        ++pos;
        int64_t right = parse_unary();
        if (error != ERR_NONE) return 0;
        if (op == '*') left = checked_mul(left, right);
        else if (op == '/') left = checked_div(left, right);
        else
        {
            if (right == 0) { error = ERR_DIV_ZERO; return 0; }
            if (left == I64_MIN && right == -1LL) { error = ERR_OVERFLOW; return 0; }
            left %= right;
        }
    }
    return left;
}

int64_t Parser::parse_add()
{
    int64_t left = parse_mul();
    while (error == ERR_NONE)
    {
        ws();
        char op = s[pos];
        if (op != '+' && op != '-') break;
        ++pos;
        int64_t right = parse_mul();
        if (error != ERR_NONE) return 0;
        left = (op == '+') ? checked_add(left, right) : checked_sub(left, right);
    }
    return left;
}

int64_t Parser::parse_shift()
{
    int64_t left = parse_add();
    while (error == ERR_NONE)
    {
        ws();
        if (s[pos] == '<' && s[pos + 1] == '<')
        {
            pos += 2;
            int64_t shift = parse_add();
            if (error != ERR_NONE) return 0;
            if (shift < 0 || shift > 63) { error = ERR_SHIFT; return 0; }
            uint64_t out = (uint64_t)left << (uint64_t)shift;
            left = (int64_t)out;
        }
        else if (s[pos] == '>' && s[pos + 1] == '>')
        {
            pos += 2;
            int64_t shift = parse_add();
            if (error != ERR_NONE) return 0;
            if (shift < 0 || shift > 63) { error = ERR_SHIFT; return 0; }
            left = (int64_t)((uint64_t)left >> (uint64_t)shift);
        }
        else break;
    }
    return left;
}

int64_t Parser::parse_and()
{
    int64_t left = parse_shift();
    while (error == ERR_NONE)
    {
        ws();
        if (s[pos] != '&') break;
        ++pos;
        left = (int64_t)((uint64_t)left & (uint64_t)parse_shift());
    }
    return left;
}

int64_t Parser::parse_xor()
{
    int64_t left = parse_and();
    while (error == ERR_NONE)
    {
        ws();
        if (s[pos] != '^') break;
        ++pos;
        left = (int64_t)((uint64_t)left ^ (uint64_t)parse_and());
    }
    return left;
}

int64_t Parser::parse_or()
{
    int64_t left = parse_xor();
    while (error == ERR_NONE)
    {
        ws();
        if (s[pos] != '|') break;
        ++pos;
        left = (int64_t)((uint64_t)left | (uint64_t)parse_xor());
    }
    return left;
}

int64_t Parser::parse_expr()
{
    return parse_or();
}

static int evaluate(const char* expression, int64_t* result, int* error)
{
    if (!expression || !result || !error) return 0;
    Parser p{expression, 0, Parser::ERR_NONE};
    int64_t value = p.parse_expr();
    p.ws();
    if (p.error == Parser::ERR_NONE && expression[p.pos] != 0)
        p.error = Parser::ERR_SYNTAX;
    *result = value;
    *error = p.error;
    return p.error == Parser::ERR_NONE;
}

static void show_error(int error)
{
    write_str("calc: ");
    switch (error)
    {
        case Parser::ERR_SYNTAX: write_str("syntax error"); break;
        case Parser::ERR_NUMBER: write_str("invalid or overflowing number"); break;
        case Parser::ERR_DIV_ZERO: write_str("division by zero"); break;
        case Parser::ERR_OVERFLOW: write_str("integer overflow"); break;
        case Parser::ERR_SHIFT: write_str("shift must be between 0 and 63"); break;
        case Parser::ERR_DOMAIN: write_str("invalid value for function"); break;
        case Parser::ERR_UNKNOWN_NAME: write_str("unknown name"); break;
        default: write_str("invalid expression"); break;
    }
    write_str("\r\n");
}

static void print_result(int64_t value)
{
    write_str("= ");
    write_i64(value);
    write_str("  [");
    write_hex64((uint64_t)value);
    write_str("]\r\n");
}

static int read_line(char* line, int cap)
{
    int len = 0;
    while (1)
    {
        char c = 0;
        int64_t got = user_read(&c, 1);
        if (got <= 0)
        {
#ifdef HOST_TEST
            continue;
#else
            asm volatile("yield");
            continue;
#endif
        }

        if (c == '\r' || c == '\n')
        {
            write_str("\r\n");
            line[len] = 0;
            return len;
        }

        if (c == 8 || c == 127)
        {
            if (len > 0)
            {
                --len;
                write_str("\b \b");
            }
            continue;
        }

        if (c >= 32 && c <= 126)
        {
            if (len < cap - 1)
            {
                line[len++] = c;
                write_char(c);
            }
        }
    }
}

static void run_expression(const char* expr)
{
    int64_t value = 0;
    int error = Parser::ERR_NONE;
    if (!evaluate(expr, &value, &error))
    {
        show_error(error);
        return;
    }
    g_ans = value;
    add_history(expr);
    print_result(value);
}

static void interactive()
{
    char line[INPUT_MAX];

    write_str("\033[2J\033[H");
    write_str("\033[1;36m========================================\033[0m\r\n");
    write_str("\033[1;36m           RioOS CALCULATOR             \033[0m\r\n");
    write_str("\033[1;36m========================================\033[0m\r\n");
    write_str("Type 'help' for commands. Timer IRQ is not used.\r\n\r\n");

    while (1)
    {
        write_str("calc> ");
        int n = read_line(line, sizeof(line));
        if (n == 0) continue;

        if (str_eq(line, "exit") || str_eq(line, "quit") || str_eq(line, "q"))
        {
            write_str("Leaving calculator.\r\n");
            return;
        }
        if (str_eq(line, "help")) { show_help(); continue; }
        if (str_eq(line, "clear")) { write_str("\033[2J\033[H"); continue; }
        if (str_eq(line, "history")) { show_history(); continue; }
        if (str_eq(line, "ans")) { print_result(g_ans); continue; }
        if (str_eq(line, "mr")) { write_str("M = "); print_result(g_mem); continue; }
        if (str_eq(line, "mc")) { g_mem = 0; write_str("Memory cleared.\r\n"); continue; }
        if (str_eq(line, "m+"))
        {
            int64_t next;
            if (__builtin_add_overflow(g_mem, g_ans, &next))
                write_str("calc: memory overflow\r\n");
            else
            {
                g_mem = next;
                write_str("M = "); print_result(g_mem);
            }
            continue;
        }
        if (str_eq(line, "m-"))
        {
            int64_t next;
            if (__builtin_sub_overflow(g_mem, g_ans, &next))
                write_str("calc: memory overflow\r\n");
            else
            {
                g_mem = next;
                write_str("M = "); print_result(g_mem);
            }
            continue;
        }
        if (str_starts(line, "m+ ")) { /* Friendly shorthand: m+ <expr> */ run_expression(line + 3); g_mem += g_ans; continue; }
        if (str_starts(line, "m- ")) { run_expression(line + 3); g_mem -= g_ans; continue; }

        run_expression(line);
    }
}

extern "C" void _start()
{
    char args[INPUT_MAX];
    args[0] = 0;
    (void)user_syscall2(USER_SYS_GETARG, (uint64_t)args, (uint64_t)sizeof(args));

    int start = 0;
    while (args[start] == ' ' || args[start] == '\t') ++start;

    if (args[start])
    {
        if (str_eq(args + start, "help") || str_eq(args + start, "-h") || str_eq(args + start, "--help"))
        {
            show_help();
        }
        else
        {
            run_expression(args + start);
        }
        user_syscall1(USER_SYS_EXIT, 0);
    }

    interactive();
    user_syscall1(USER_SYS_EXIT, 0);
    while (1)
    {
#ifdef HOST_TEST
        return;
#else
        asm volatile("wfe");
#endif
    }
}
