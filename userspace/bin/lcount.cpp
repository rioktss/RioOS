#include "../libc/ulibc.h"
/* counts lines/words/bytes on stdin (pipeline stage) */
int main(int argc, char** argv)
{
    (void)argc; (void)argv;
    char buf[128]; long n; unsigned lines = 0, words = 0, bytes = 0; int in = 0;
    while ((n = read(0, buf, sizeof(buf))) != 0)
    {
        if (n < 0) { if (errno == EAGAIN) { sched_yield(); continue; } return 1; }
        for (long i = 0; i < n; ++i)
        {
            ++bytes; if (buf[i] == '\n') ++lines;
            if (isspace(buf[i])) in = 0; else if (!in) { in = 1; ++words; }
        }
    }
    printf("%u lines, %u words, %u bytes\n", lines, words, bytes);
    return 0;
}
