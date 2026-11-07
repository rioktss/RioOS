#include "../libc/ulibc.h"
/* proctest: fork/exec/env/signals/shared mmap/cloexec/system/popen/posix_spawn checks (POSIX process layer).
   Run as `proctest`; the same binary re-executes itself as a child with a mode in argv[1]. */
static int fails = 0;
static void chk(const char* name, int ok) { printf("%s %s\n", ok ? "PASS" : "FAIL", name); if (!ok) ++fails; }

static volatile int got_sig = 0, got_cnt = 0;
static void on_usr1(int s) { got_sig = s; ++got_cnt; }
static volatile int chld_seen = 0;
static void on_chld(int s) { (void)s; ++chld_seen; }
static volatile int in_handler_mask_ok = 0;
static void on_usr2(int s) { (void)s; sigset_t cur; sigprocmask(SIG_BLOCK, 0, &cur); in_handler_mask_ok = sigismember(&cur, SIGUSR2) == 1; }

static int child_main(int argc, char** argv)
{
    const char* mode = argv[1];
    if (!strcmp(mode, "env"))
    {
        printf("ARGC=%d A0=%s A1=%s A2=%s FOO=%s BAR=%s\n", argc, argv[0], argv[1], argc > 2 ? argv[2] : "-", getenv("FOO") ? getenv("FOO") : "(null)", getenv("BAR") ? getenv("BAR") : "(null)");
        return 7;
    }
    if (!strcmp(mode, "exit")) return atoi(argv[2]);
    if (!strcmp(mode, "sigdfl"))
    {   /* handler installed before exec must be default again; ignored stays ignored */
        struct sigaction o; sigaction(SIGUSR1, 0, &o);
        printf("USR1=%s USR2=%s\n", o.sa_handler == SIG_DFL ? "dfl" : "other", "x");
        sigaction(SIGUSR2, 0, &o);
        printf("USR2=%s\n", o.sa_handler == SIG_IGN ? "ign" : "other");
        return 0;
    }
    if (!strcmp(mode, "cloexec"))
    {
        char c;
        int a = (int)read(atoi(argv[2]), &c, 1);
        int b = (int)read(atoi(argv[3]), &c, 1);
        printf("keep=%s closed=%s\n", a >= 0 || errno != EBADF ? "open" : "BAD", b < 0 && errno == EBADF ? "closed" : "STILL-OPEN");
        return 0;
    }
    if (!strcmp(mode, "waitsig")) { for (;;) pause(); }
    if (!strcmp(mode, "shmwrite"))
    {
        int fd = open("/pt_shared.bin", O_RDWR);
        char* m = (char*)mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (m == MAP_FAILED) return 2;
        strcpy(m + 100, "from-other-process");
        close(fd);
        return 0;   /* exit writes the page back */
    }
    return 99;
}

int main(int argc, char** argv)
{
    if (argc >= 2) return child_main(argc, argv);
    char buf[256]; int st;

    /* ---- fork / wait / exit status ---- */
    pid_t p = fork();
    if (p == 0) _exit(42);
    chk("fork-wait-exit", waitpid(p, &st, 0) == p && WIFEXITED(st) && WEXITSTATUS(st) == 42);
    chk("wait-no-child", wait(&st) < 0 && errno == ECHILD);

    /* ---- execve: argv[0], args with spaces, environment ---- */
    p = fork();
    if (p == 0)
    {
        char* av[] = { (char*)"custom0", (char*)"env", (char*)"two words", 0 };
        char* ev[] = { (char*)"FOO=hello", (char*)"BAR=a b", 0 };
        execve("/bin/proctest.elf", av, ev);
        execvp("proctest", av);
        _exit(111);
    }
    chk("execve-status", waitpid(p, &st, 0) == p && WEXITSTATUS(st) == 7);

    /* output capture of env child through popen */
    FILE* f = popen("proctest env x", "r");
    chk("popen-open", f != 0);
    buf[0] = 0; if (f) { fgets(buf, sizeof(buf), f); }
    chk("popen-output", strstr(buf, "A1=env") && strstr(buf, "A2=x"));
    chk("popen-status", f && pclose(f) == (7 << 8));

    /* environment inheritance across fork+execv */
    setenv("FOO", "inherited", 1);
    int pf[2]; pipe(pf);
    p = fork();
    if (p == 0) { dup2(pf[1], 1); close(pf[0]); close(pf[1]); char* av[] = { (char*)"proctest", (char*)"env", 0 }; execv("/bin/proctest.elf", av); _exit(111); }
    close(pf[1]); { int n = (int)read(pf[0], buf, sizeof(buf) - 1); buf[n > 0 ? n : 0] = 0; } close(pf[0]);
    waitpid(p, &st, 0);
    chk("env-inherited", strstr(buf, "FOO=inherited") != 0);

    /* execvp of a missing program fails with ENOENT */
    chk("exec-missing", execvp("no_such_prog_xyz", argv) < 0 && errno == ENOENT);

    /* ---- system() ---- */
    chk("system-exit", system("proctest exit 3") == (3 << 8));
    chk("system-and", system("proctest exit 0 && proctest exit 5") == (5 << 8));
    chk("system-or", system("proctest exit 1 || proctest exit 6") == (6 << 8));
    chk("system-notfound", system("no_such_prog_xyz") == (127 << 8));
    system("proctest env redir > /pt_out.txt");
    f = fopen("/pt_out.txt", "r"); buf[0] = 0; if (f) { fgets(buf, sizeof(buf), f); fclose(f); }
    chk("system-redirect", strstr(buf, "A1=env") && strstr(buf, "A2=redir"));

    /* ---- posix_spawn ---- */
    { posix_spawn_file_actions_t fa; pid_t sp = 0; char* av[] = { (char*)"proctest", (char*)"exit", (char*)"9", 0 };
      posix_spawn_file_actions_init(&fa);
      int rc = posix_spawnp(&sp, "proctest", &fa, 0, av, environ);
      chk("posix_spawn", rc == 0 && waitpid(sp, &st, 0) == sp && WEXITSTATUS(st) == 9); }

    /* ---- signals: handler, mask, ignore ---- */
    { struct sigaction sa; sa.sa_handler = on_usr1; sa.sa_mask = 0; sa.sa_flags = 0;
      chk("sigaction", sigaction(SIGUSR1, &sa, 0) == 0);
      raise(SIGUSR1);
      chk("handler-runs", got_sig == SIGUSR1 && got_cnt == 1);
      sigset_t m, old; sigemptyset(&m); sigaddset(&m, SIGUSR1);
      sigprocmask(SIG_BLOCK, &m, &old);
      raise(SIGUSR1);
      chk("blocked-not-delivered", got_cnt == 1);
      sigset_t pend; sigpending(&pend);
      chk("sigpending", sigismember(&pend, SIGUSR1) == 1);
      sigprocmask(SIG_SETMASK, &old, 0);
      chk("unblock-delivers", got_cnt == 2);
      sa.sa_handler = on_usr2; sigaction(SIGUSR2, &sa, 0);
      raise(SIGUSR2);
      chk("signal-blocked-in-handler", in_handler_mask_ok == 1);
      sigset_t after; sigprocmask(SIG_BLOCK, 0, &after);
      chk("mask-restored-after-handler", sigismember(&after, SIGUSR2) == 0);
      sa.sa_handler = SIG_IGN; sigaction(SIGUSR1, &sa, 0);
      raise(SIGUSR1);
      chk("ignored", got_cnt == 2);
      chk("signal()-returns-old", signal(SIGUSR2, SIG_DFL) == on_usr2);
    }

    /* ---- alarm + pause ---- */
    { signal(SIGALRM, on_usr1); int before = got_cnt; alarm(1); int r = pause();
      chk("alarm-pause", r == -1 && errno == EINTR && got_cnt == before + 1);
      alarm(5); chk("alarm-cancel-returns-left", alarm(0) >= 4); }

    /* ---- kill between processes + handler in child, SIGCHLD ---- */
    { signal(SIGCHLD, on_chld);
      p = fork();
      if (p == 0) { signal(SIGUSR1, on_usr1); int c0 = got_cnt; while (got_cnt == c0) pause(); _exit(got_sig == SIGUSR1 ? 21 : 22); }
      sleep_ms(60); kill(p, SIGUSR1);
      chk("child-handler-via-kill", waitpid(p, &st, 0) == p && WEXITSTATUS(st) == 21);
      chk("sigchld-handler", chld_seen >= 1);
      signal(SIGCHLD, SIG_DFL);
      p = fork();
      if (p == 0) { signal(SIGTERM, SIG_DFL); for (;;) pause(); }
      sleep_ms(60); kill(p, SIGTERM);
      chk("default-terminate", waitpid(p, &st, 0) == p && WIFSIGNALED(st) && WTERMSIG(st) == SIGTERM);
      chk("kill-esrch", kill(p, 0) < 0); }

    /* handlers reset by exec, ignored kept */
    { signal(SIGUSR1, on_usr1); signal(SIGUSR2, SIG_IGN);
      pf[0] = pf[1] = -1; pipe(pf);
      p = fork();
      if (p == 0) { dup2(pf[1], 1); close(pf[0]); close(pf[1]); char* av[] = { (char*)"proctest", (char*)"sigdfl", 0 }; execv("/bin/proctest.elf", av); _exit(111); }
      close(pf[1]); { int n = (int)read(pf[0], buf, sizeof(buf) - 1); buf[n > 0 ? n : 0] = 0; } close(pf[0]);
      waitpid(p, &st, 0);
      chk("exec-resets-handlers", strstr(buf, "USR1=dfl") != 0);
      chk("exec-keeps-ignored", strstr(buf, "USR2=ign") != 0); }

    /* ---- FD_CLOEXEC ---- */
    { int a = open("/pt_out.txt", O_RDONLY), b = open("/pt_out.txt", O_RDONLY | O_CLOEXEC);
      chk("getfd", fcntl(a, F_GETFD) == 0 && fcntl(b, F_GETFD) == FD_CLOEXEC);
      int c = open("/pt_out.txt", O_RDONLY); fcntl(c, F_SETFD, FD_CLOEXEC);
      chk("setfd", fcntl(c, F_GETFD) == FD_CLOEXEC);
      int d = fcntl(a, F_DUPFD, 10); chk("dupfd", d >= 10 && fcntl(d, F_GETFD) == 0);
      pf[0] = pf[1] = -1; pipe(pf);
      char sa_[16], sb_[16]; sprintf(sa_, "%d", a); sprintf(sb_, "%d", b);
      p = fork();
      if (p == 0) { dup2(pf[1], 1); close(pf[0]); close(pf[1]); char* av[] = { (char*)"proctest", (char*)"cloexec", sa_, sb_, 0 }; execv("/bin/proctest.elf", av); _exit(111); }
      close(pf[1]); { int n = (int)read(pf[0], buf, sizeof(buf) - 1); buf[n > 0 ? n : 0] = 0; } close(pf[0]);
      waitpid(p, &st, 0);
      chk("cloexec-closed-on-exec", strstr(buf, "keep=open") && strstr(buf, "closed=closed"));
      close(a); close(b); close(c); close(d); }

    /* ---- shared anonymous mmap across fork ---- */
    { volatile int* shm = (volatile int*)mmap(0, 8192, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
      chk("mmap-shared-anon", shm != MAP_FAILED);
      shm[0] = 1;
      p = fork();
      if (p == 0) { shm[0] = 123; shm[1025] = 77; _exit(0); }
      waitpid(p, &st, 0);
      chk("shared-anon-visible-to-parent", shm[0] == 123 && shm[1025] == 77);
      int* priv = (int*)mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
      priv[0] = 5; p = fork();
      if (p == 0) { priv[0] = 6; _exit(0); }
      waitpid(p, &st, 0);
      chk("private-anon-isolated", priv[0] == 5);
      munmap((void*)shm, 8192); munmap(priv, 4096); }

    /* ---- shared file mmap: write-through to file, visibility across processes ---- */
    { f = fopen("/pt_shared.bin", "w"); for (int i = 0; i < 6000; ++i) fputc('A' + i % 26, f); fclose(f);
      int fd = open("/pt_shared.bin", O_RDWR);
      char* m = (char*)mmap(0, 6000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
      chk("mmap-file-shared", m != MAP_FAILED && m[0] == 'A' && m[27] == 'B' && m[5999] == (char)('A' + 5999 % 26));
      m[10] = '#'; m[5000] = '@';
      chk("msync", msync(m, 6000, MS_SYNC) == 0);
      f = fopen("/pt_shared.bin", "r"); fseek(f, 10, SEEK_SET); int c1 = fgetc(f); fseek(f, 5000, SEEK_SET); int c2 = fgetc(f); fclose(f);
      chk("msync-written-to-file", c1 == '#' && c2 == '@');
      p = fork();   /* unrelated process maps the same file and writes; parent sees it live */
      if (p == 0) { char* av[] = { (char*)"proctest", (char*)"shmwrite", 0 }; execv("/bin/proctest.elf", av); _exit(111); }
      waitpid(p, &st, 0);
      chk("shared-file-live-across-processes", strcmp(m + 100, "from-other-process") == 0);
      munmap(m, 6000);
      f = fopen("/pt_shared.bin", "r"); fseek(f, 100, SEEK_SET); char tmp[32] = {0}; fread(tmp, 1, 18, f); fclose(f);
      chk("exit-writeback-persisted", strcmp(tmp, "from-other-process") == 0);
      int ro = open("/pt_shared.bin", O_RDONLY);
      chk("shared-write-needs-rdwr", mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, ro, 0) == MAP_FAILED && errno == EACCES);
      chk("shared-ro-ok", mmap(0, 4096, PROT_READ, MAP_SHARED, ro, 0) != MAP_FAILED);
      chk("map-fixed-rejected", mmap(0, 4096, PROT_READ, MAP_PRIVATE | MAP_FIXED | MAP_ANONYMOUS, -1, 0) == MAP_FAILED);
      close(ro); close(fd); }

    printf("proctest done fails=%d\n", fails);
    return fails ? 1 : 0;
}
