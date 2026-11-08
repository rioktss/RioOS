/*
 * MIT License
 *
 * Copyright (c) 2026 Rio okta kusuma
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files, to deal in the Software
 * without restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies of the
 * Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

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
