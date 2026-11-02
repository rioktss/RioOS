#include "../libc/ulibc.h"
int main(int argc, char** argv)
{
    printf("hello from pid %d (ppid %d), argc=%d\n", getpid(), getppid(), argc);
    for (int i = 1; i < argc; ++i) printf("  arg[%d]=%s\n", i, argv[i]);
    return argc > 1 ? 7 : 0;
}
