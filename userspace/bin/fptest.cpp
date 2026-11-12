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
/* fptest: FP/NEON state is per-thread/process: context switch, fork, exec, signals, setjmp, green threads.
   All vector work is inline asm (the user libc ABI is soft-float; the compiler never touches V registers). */
static int fails = 0;
static void chk(const char* name, int ok) { printf("%s %s\n", ok ? "PASS" : "FAIL", name); if (!ok) ++fails; }

/* vN <- { seed + N*0x111, ~(seed + N*0x1111) } for N = 0..31 */
static void fp_fill(unsigned long seed)
{
    asm volatile(
        ".arch_extension simd\n"
        "mov x9, %0\n"
        ".irp n,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31\n"
        "  mvn x10, x9\n"
        "  fmov d\\n, x9\n"
        "  mov v\\n\\().d[1], x10\n"
        "  add x9, x9, #0x111\n"
        ".endr\n"
        :: "r"(seed) : "x9", "x10", "memory");
}

/* number of vector registers (of 32) that differ from the pattern for seed */
static int fp_diff(unsigned long seed)
{
    unsigned long bad = 0;
    asm volatile(
        ".arch_extension simd\n"
        "mov x9, %1\n"
        "mov %0, xzr\n"
        ".irp n,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31\n"
        "  mvn x10, x9\n"
        "  umov x11, v\\n\\().d[0]\n"
        "  umov x12, v\\n\\().d[1]\n"
        "  eor x11, x11, x9\n"
        "  eor x12, x12, x10\n"
        "  orr x11, x11, x12\n"
        "  cmp x11, #0\n"
        "  cinc %0, %0, ne\n"
        "  add x9, x9, #0x111\n"
        ".endr\n"
        : "=&r"(bad) : "r"(seed) : "x9", "x10", "x11", "x12", "cc", "memory");
    return (int)bad;
}

static int fp_zero_count(void)
{
    unsigned long bad = 0;
    asm volatile(
        ".arch_extension simd\n"
        "mov %0, xzr\n"
        ".irp n,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31\n"
        "  umov x11, v\\n\\().d[0]\n"
        "  umov x12, v\\n\\().d[1]\n"
        "  orr x11, x11, x12\n"
        "  cmp x11, #0\n"
        "  cinc %0, %0, ne\n"
        ".endr\n"
        : "=&r"(bad) :: "x11", "x12", "cc", "memory");
    return (int)bad;
}

/* d8..d15 low halves only (the AAPCS callee-saved part) */
static void fp_set_callee(unsigned long seed)
{
    asm volatile(
        ".arch_extension simd\n"
        "mov x9, %0\n"
        ".irp n,8,9,10,11,12,13,14,15\n"
        "  fmov d\\n, x9\n"
        "  add x9, x9, #0x77\n"
        ".endr\n"
        :: "r"(seed) : "x9", "memory");
}
static int fp_callee_diff(unsigned long seed)
{
    unsigned long bad = 0;
    asm volatile(
        ".arch_extension simd\n"
        "mov x9, %1\n"
        "mov %0, xzr\n"
        ".irp n,8,9,10,11,12,13,14,15\n"
        "  fmov x11, d\\n\n"
        "  cmp x11, x9\n"
        "  cinc %0, %0, ne\n"
        "  add x9, x9, #0x77\n"
        ".endr\n"
        : "=&r"(bad) : "r"(seed) : "x9", "x11", "cc", "memory");
    return (int)bad;
}
/* clobber every vector register with a different pattern (used inside signal handlers / other threads) */
static void fp_clobber(void) { fp_fill(0xdeadbeef0000ULL); }

/* real arithmetic: 1.5 * 2.0 + 0.25 == 3.25 using hardware double ops */
static int fp_math_ok(void)
{
    unsigned long out = 0;
    asm volatile(
        ".arch_extension fp\n"
        "fmov d0, #1.5\n"
        "fmov d1, #2.0\n"
        "fmul d0, d0, d1\n"
        "fmov d2, #0.25\n"
        "fadd d0, d0, d2\n"
        "fmov %0, d0\n"
        : "=r"(out) :: "memory");
    return out == 0x400A000000000000UL;   /* 3.25 */
}
/* FPCR rounding mode / FPSR are per process too */
static unsigned long rd_fpcr(void) { unsigned long v; asm volatile(".arch_extension fp\n mrs %0, fpcr" : "=r"(v)); return v; }
static void wr_fpcr(unsigned long v) { asm volatile(".arch_extension fp\n msr fpcr, %0" :: "r"(v)); }

static volatile int g_sig = 0;
static void on_usr1(int s) { g_sig = s; fp_clobber(); wr_fpcr(0); }

static jmp_buf jb;
static volatile int g_thr_bad = 0;
static void* thr_fn(void* arg)
{
    unsigned long seed = (unsigned long)arg;
    fp_set_callee(seed);
    for (int i = 0; i < 200; ++i) {
        pthread_yield();
        if (fp_callee_diff(seed)) ++g_thr_bad;
    }
    return 0;
}

static void spin_ticks(int n)   /* burn CPU so the 100 Hz timer preempts us */
{
    unsigned long t0 = (unsigned long)clock();
    (void)t0;
    for (volatile unsigned long i = 0; i < (unsigned long)n * 300000UL; ++i) { }
}

int main(int argc, char** argv)
{
    if (argc > 1 && strcmp(argv[1], "zero") == 0)        /* exec target: registers must be zero */
        return fp_zero_count() == 0 ? 0 : 1;
    if (argc > 1 && strcmp(argv[1], "fpcr0") == 0)
        return rd_fpcr() == 0 ? 0 : 1;

    chk("hw-double-arith", fp_math_ok());
    fp_fill(0x1000);
    chk("fill-readback", fp_diff(0x1000) == 0);

    /* ---- context switching: several processes, each with its own register image, running concurrently ---- */
    enum { NP = 4 };
    pid_t kids[NP];
    for (int k = 0; k < NP; ++k) {
        pid_t p = fork();
        if (p == 0) {
            unsigned long seed = 0x100000UL * (k + 2);
            int bad = 0;
            fp_fill(seed);
            for (int r = 0; r < 60; ++r) {
                spin_ticks(2);
                if (fp_diff(seed)) ++bad;
                if (r % 7 == 0) sched_yield();
                if (fp_diff(seed)) ++bad;
            }
            _exit(bad > 120 ? 120 : bad);
        }
        kids[k] = p;
    }
    int allok = 1;
    for (int r = 0; r < 40; ++r) { spin_ticks(2); if (fp_diff(0x1000)) allok = 0; }
    chk("parent-regs-survive-switches", allok);
    for (int k = 0; k < NP; ++k) {
        int st = 0; waitpid(kids[k], &st, 0);
        char nm[48]; snprintf(nm, sizeof nm, "child%d-regs-isolated", k);
        chk(nm, WIFEXITED(st) && WEXITSTATUS(st) == 0);
    }

    /* ---- fork copies the register file ---- */
    fp_fill(0x2222);
    pid_t p = fork();
    if (p == 0) _exit(fp_diff(0x2222) == 0 ? 0 : 1);
    int st = 0; waitpid(p, &st, 0);
    chk("fork-inherits-regs", WIFEXITED(st) && WEXITSTATUS(st) == 0);
    chk("fork-parent-unchanged", fp_diff(0x2222) == 0);

    /* ---- exec starts with zeroed registers and FPCR=0 ---- */
    fp_fill(0x3333); wr_fpcr(0x400000);               /* set FZ so FPCR is observably non-zero */
    p = fork();
    if (p == 0) { char* a[] = { argv[0], (char*)"zero", 0 }; execv("/bin/fptest.elf", a); _exit(99); }
    waitpid(p, &st, 0);
    chk("exec-zeroes-regs", WIFEXITED(st) && WEXITSTATUS(st) == 0);
    p = fork();
    if (p == 0) { char* a[] = { argv[0], (char*)"fpcr0", 0 }; execv("/bin/fptest.elf", a); _exit(99); }
    waitpid(p, &st, 0);
    chk("exec-resets-fpcr", WIFEXITED(st) && WEXITSTATUS(st) == 0);
    chk("fpcr-kept-in-parent", rd_fpcr() == 0x400000UL);
    wr_fpcr(0);

    /* ---- signals: handler may clobber everything, sigreturn must restore the interrupted state ---- */
    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_handler = on_usr1; sigaction(SIGUSR1, &sa, 0);
    fp_fill(0x4444); wr_fpcr(0x400000);
    g_sig = 0; kill(getpid(), SIGUSR1);
    for (int i = 0; i < 1000 && !g_sig; ++i) sched_yield();
    chk("signal-delivered", g_sig == SIGUSR1);
    chk("sigreturn-restores-vregs", fp_diff(0x4444) == 0);
    chk("sigreturn-restores-fpcr", rd_fpcr() == 0x400000UL);
    wr_fpcr(0);

    /* ---- setjmp/longjmp keep d8-d15 ---- */
    fp_set_callee(0x5000);
    if (setjmp(jb) == 0) { fp_clobber(); longjmp(jb, 1); }
    chk("longjmp-restores-d8-d15", fp_callee_diff(0x5000) == 0);

    /* ---- green threads keep their own d8-d15 across switches ---- */
    fp_set_callee(0x6000);
    pthread_t t1, t2;
    pthread_create(&t1, 0, thr_fn, (void*)0x7000UL);
    pthread_create(&t2, 0, thr_fn, (void*)0x8000UL);
    for (int i = 0; i < 100; ++i) { pthread_yield(); if (fp_callee_diff(0x6000)) ++g_thr_bad; }
    pthread_join(t1, 0); pthread_join(t2, 0);
    chk("pthread-switch-keeps-d8-d15", g_thr_bad == 0);

    printf("fptest done fails=%d\n", fails);
    return fails ? 1 : 0;
}
