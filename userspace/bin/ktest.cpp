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
/* In-OS regression suite for multitasking, memory and IPC. */
static int fails = 0, total = 0;
#define CHECK(c) do { ++total; if (c) printf("[PASS] %s\n", #c); else { ++fails; printf("[FAIL] %s (line %d)\n", #c, __LINE__); } } while (0)

static int g_value = 100;
static char g_big[3 * 4096];

int main(int argc, char** argv)
{
    (void)argc; (void)argv;
    /* fork + wait + exit status */
    pid_t c = fork();
    if (c == 0) _exit(42);
    int st = 0;
    CHECK(c > 0);
    CHECK(waitpid(c, &st, 0) == c);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 42);

    /* copy-on-write: child writes do not reach the parent */
    int local = 5;
    g_big[4096] = 'P';
    c = fork();
    if (c == 0)
    {
        g_value = 999; local = 77; g_big[4096] = 'C';
        _exit(g_value == 999 && local == 77 && g_big[4096] == 'C' ? 0 : 1);
    }
    CHECK(waitpid(c, &st, 0) == c && WEXITSTATUS(st) == 0);
    CHECK(g_value == 100 && local == 5 && g_big[4096] == 'P');

    /* several children running concurrently (preemptive) */
    pid_t kids[4];
    for (int i = 0; i < 4; ++i)
    {
        kids[i] = fork();
        if (kids[i] == 0) { unsigned long t = clock_ms(); while (clock_ms() - t < 150) {} _exit(10 + i); }
    }
    int ok = 1;
    for (int i = 0; i < 4; ++i) { if (waitpid(kids[i], &st, 0) != kids[i] || WEXITSTATUS(st) != 10 + i) ok = 0; }
    CHECK(ok);
    CHECK(wait(&st) == -1 && errno == ECHILD);

    /* exec replaces the image */
    c = fork();
    if (c == 0) { execv_args("/bin/hello.elf", "a b"); _exit(99); }
    CHECK(waitpid(c, &st, 0) == c && WEXITSTATUS(st) == 7);
    c = fork();
    if (c == 0) { execv_args("/bin/does-not-exist.elf", ""); _exit(55); }
    CHECK(waitpid(c, &st, 0) == c && WEXITSTATUS(st) == 55);

    /* blocking pipe between parent and child */
    int p[2];
    CHECK(pipe(p) == 0);
    c = fork();
    if (c == 0)
    {
        close(p[0]);
        for (int i = 0; i < 3; ++i) { sleep_ms(30); write(p[1], "ping ", 5); }
        close(p[1]);
        _exit(0);
    }
    close(p[1]);
    char buf[64]; int got = 0; long n;
    while ((n = read(p[0], buf + got, sizeof(buf) - 1 - (size_t)got)) != 0) { if (n > 0) got += (int)n; else if (errno != EAGAIN) { printf("pipe read err n=%ld errno=%d\n", n, errno); break; } }
    buf[got] = 0;
    printf("pipe got=%d [%s]\n", got, buf);
    CHECK(got == 15 && strcmp(buf, "ping ping ping ") == 0);
    waitpid(c, &st, 0);

    /* signals */
    c = fork();
    if (c == 0) { for (;;) sched_yield(); }
    sleep_ms(50);
    CHECK(kill(c, SIGTERM) == 0);
    CHECK(waitpid(c, &st, 0) == c && WIFSIGNALED(st) && WTERMSIG(st) == SIGTERM);
    c = fork();
    if (c == 0) { volatile unsigned long x = 0; for (;;) ++x; }   /* CPU bound: only preemption lets us kill it */
    sleep_ms(50);
    CHECK(kill(c, SIGKILL) == 0);
    CHECK(waitpid(c, &st, 0) == c && WTERMSIG(st) == SIGKILL);

    /* memory: brk, malloc, mmap, demand paging */
    void* a = malloc(100); void* b = malloc(200);
    CHECK(a && b && a != b);
    memset(a, 0xAB, 100);
    free(a); free(b);
    char* big = (char*)mmap(0, 256 * 1024, PROT_READ | PROT_WRITE, 0, -1, 0);
    CHECK(big != MAP_FAILED);
    big[0] = 1; big[128 * 1024] = 2; big[256 * 1024 - 1] = 3;
    CHECK(big[0] == 1 && big[128 * 1024] == 2 && big[256 * 1024 - 1] == 3 && big[5000] == 0);
    CHECK(munmap(big, 256 * 1024) == 0);
    char* hp = (char*)malloc(300000);        /* > 64 KiB -> mmap-backed */
    CHECK(hp != 0);
    if (hp) { hp[299999] = 9; CHECK(hp[299999] == 9); free(hp); }

    /* spawn + redirected pipeline: hello | upper style via spawn_fd */
    int q[2];
    CHECK(pipe(q) == 0);
    pid_t s = spawn_fd("/bin/hello.elf", "world", -1, q[1]);
    CHECK(s > 0);
    close(q[1]);
    got = 0;
    while ((n = read(q[0], buf + got, sizeof(buf) - 1 - (size_t)got)) != 0) { if (n > 0) got += (int)n; else if (errno != EAGAIN) break; }
    buf[got] = 0;
    CHECK(strstr(buf, "arg[1]=world") != 0);
    CHECK(waitpid(s, &st, 0) == s);

    /* privilege separation */
    if (getuid() != 0)
    {
        CHECK(getuid() >= 1000);
        CHECK(open("/etc/passwd", 0) < 0);
        CHECK(setuid(0, 0) < 0);
    }
    else
    {
        CHECK(open("/etc/passwd", 0) >= 0);
    }

    printf("ktest: %d/%d passed\n", total - fails, total);
    return fails ? 1 : 0;
}
