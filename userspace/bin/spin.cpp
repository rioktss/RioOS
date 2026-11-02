#include "../libc/ulibc.h"
/* CPU-bound task: proves preemption — prints progress without ever yielding. */
int main(int argc, char** argv)
{
    unsigned long ms = argc > 1 ? (unsigned long)atoi(argv[1]) : 1000;
    unsigned long start = clock_ms(), last = start; unsigned long counter = 0;
    while (clock_ms() - start < ms)
    {
        ++counter;
        if (clock_ms() - last >= 250) { last = clock_ms(); printf("[spin %d] %lu ms\n", getpid(), last - start); }
    }
    printf("[spin %d] done (%lu iterations)\n", getpid(), counter);
    return 0;
}
