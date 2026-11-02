#include "../libc/ulibc.h"
int main(int argc, char** argv)
{
    (void)argc; (void)argv;
    void* h = dlopen("/bin/libhello.so", 0);
    if (!h) { printf("dlopen failed: %s\n", dlerror()); return 1; }
    int (*add)(int, int) = (int (*)(int, int))dlsym(h, "libhello_add");
    const char* (*name)(int) = (const char* (*)(int))dlsym(h, "libhello_name");
    int (*bump)(void) = (int (*)(void))dlsym(h, "libhello_bump");
    void (*greet)(const char*) = (void (*)(const char*))dlsym(h, "libhello_greet");
    int (*call)(int, int, int) = (int (*)(int, int, int))dlsym(h, "libhello_call");
    if (!add || !name || !bump || !greet || !call) { printf("dlsym failed: %s\n", dlerror()); return 1; }
    int ok = 1;
    ok &= add(2, 3) == 15;                   /* 2+3+counter(10) */
    ok &= bump() == 11 && add(0, 0) == 11;   /* writable data lives in the loaded image */
    ok &= strcmp(name(1), "beta") == 0;      /* RELATIVE relocation through a pointer table */
    ok &= call(1, 6, 7) == 42 && call(0, 1, 1) == 13;   /* function-pointer table */
    greet("dltest");                         /* imports printf/getpid from the host program */
    printf("dltest add=%d name=%s -> %s\n", add(1, 1), name(2), ok ? "PASS" : "FAIL");
    ok &= dlsym(h, "no_such_symbol") == 0;
    ok &= dlclose(h) == 0;
    void* bad = dlopen("/bin/hello.elf", 0);
    ok &= bad == 0;
    printf("dltest %s\n", ok ? "ALL PASS" : "FAILED");
    return ok ? 0 : 1;
}
