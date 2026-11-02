#include "../libc/ulibc.h"
/* stdin -> upper-case -> stdout (pipeline stage) */
int main(int argc, char** argv)
{
    (void)argc; (void)argv;
    char buf[128];
    long n;
    while ((n = read(0, buf, sizeof(buf))) != 0)
    {
        if (n < 0) { if (errno == EAGAIN) { sched_yield(); continue; } return 1; }
        for (long i = 0; i < n; ++i) buf[i] = (char)toupper(buf[i]);
        write(1, buf, (size_t)n);
    }
    return 0;
}
