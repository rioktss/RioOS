#include "memory.h"
#include "jobs.h"
#include "script.h"
#include "task.h"
#include "process.h"
#include "fs.h"
#include "fd.h"
#include "uart.h"
#include "string.h"
#include "user.h"
#include "mmu.h"
#include "timer.h"
#include "thread.h"
#include "security.h"
#include "vfs.h"

#define JOBS_MAX 16
#define SHELL_PID 1

struct Job
{
    int used;
    int id;
    int pid;
    int stopped;
    char cmd[48];
};

static Job jobs[JOBS_MAX];
static int next_job_id = 1;
static uint8_t probe[65536];

static void put_int(int v)
{
    if (v < 0) { uart_putc('-'); v = -v; }
    char b[12]; int n = 0;
    do { b[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 11);
    while (n) uart_putc(b[--n]);
}

static int is_elf_file(const char* path)
{
    int id = fs_resolve(path, fs_get_root());
    if (id < 0 || fs_get_type(id) != FS_FILE) return 0;
    uint64_t size = 0;
    uint64_t fsz = fs_file_size(id);
    if (fsz < 4) return 0;
    if (fsz > sizeof(probe))                                   /* big program: read into a temporary heap buffer */
    {
        uint8_t* big = (uint8_t*)kmalloc(fsz);
        if (!big) return 0;
        int ok = fs_read_file(path, fs_get_root(), big, fsz, &size) == 0 && size >= 4 && big[0] == 0x7F && big[1] == 'E' && big[2] == 'L' && big[3] == 'F';
        kfree(big);
        return ok;
    }
    if (fs_read_file(path, fs_get_root(), probe, sizeof(probe), &size) != 0 || size < 4) return 0;
    return probe[0] == 0x7F && probe[1] == 'E' && probe[2] == 'L' && probe[3] == 'F';
}

int jobs_resolve_program(const char* name, char* out, int cap)
{
    if (!name || !name[0]) return 0;
    if (name[0] == '/')
    {
        str_copy(out, name, cap);
        return is_elf_file(out);
    }
    char t[96];
    const char* prefix = (name[0] == '.' && name[1] == '/') ? 0 : "/bin/";
    if (prefix) { str_copy(t, prefix, sizeof(t)); int l = str_len(t); str_copy(t + l, name, (int)sizeof(t) - l); }
    else
    {
        /* ./prog relative to the current directory */
        char cwd[96]; vfs_get_path(cwd, sizeof(cwd));
        int l = str_len(cwd);
        str_copy(t, cwd, sizeof(t));
        if (l > 0 && t[l - 1] != '/' && l < (int)sizeof(t) - 1) { t[l++] = '/'; t[l] = 0; }
        str_copy(t + l, name + 2, (int)sizeof(t) - l);
    }
    if (is_elf_file(t)) { str_copy(out, t, cap); return 1; }
    int l = str_len(t);
    if (l < 90) { t[l] = '.'; t[l+1] = 'e'; t[l+2] = 'l'; t[l+3] = 'f'; t[l+4] = 0; if (is_elf_file(t)) { str_copy(out, t, cap); return 1; } }
    return 0;
}

static int split_stage(char* s, char** prog, char** args)
{
    while (*s == ' ') ++s;
    *prog = s;
    while (*s && *s != ' ') ++s;
    if (*s) { *s++ = 0; while (*s == ' ') ++s; }
    *args = s;
    { int l = str_len(s); while (l > 0 && s[l - 1] == ' ') s[--l] = 0; }
    return 1;
}

static Job* job_add(int pid, const char* cmd)
{
    for (int i = 0; i < JOBS_MAX; ++i)
        if (!jobs[i].used)
        {
            jobs[i].used = 1; jobs[i].id = next_job_id++; jobs[i].pid = pid; jobs[i].stopped = 0;
            str_copy(jobs[i].cmd, cmd, sizeof(jobs[i].cmd));
            return &jobs[i];
        }
    return 0;
}

static Job* job_find(const char* spec)
{
    if (!spec || !spec[0])
    {
        Job* best = 0;
        for (int i = 0; i < JOBS_MAX; ++i) if (jobs[i].used && (!best || jobs[i].id > best->id)) best = &jobs[i];
        return best;
    }
    int is_id = spec[0] == '%';
    const char* p = is_id ? spec + 1 : spec;
    int v = 0; int any = 0;
    while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); ++p; any = 1; }
    if (!any || *p) return 0;
    for (int i = 0; i < JOBS_MAX; ++i)
        if (jobs[i].used && (is_id ? jobs[i].id == v : jobs[i].pid == v)) return &jobs[i];
    return 0;
}

static const char* signal_name(int sig)
{
    switch (sig) { case 2: return "Interrupt"; case 9: return "Killed"; case 15: return "Terminated";
                   case 11: return "Segmentation fault"; case 13: return "Broken pipe"; default: return "Signal"; }
}

void jobs_poll()
{
    for (int i = 0; i < JOBS_MAX; ++i)
    {
        if (!jobs[i].used) continue;
        int st = process_get_state(jobs[i].pid);
        if (st == PROCESS_ZOMBIE || st < 0)
        {
            int code = st < 0 ? 0 : process_exit_code(jobs[i].pid);
            int sig = st < 0 ? 0 : process_term_signal(jobs[i].pid);
            uart_puts("["); put_int(jobs[i].id); uart_puts("]+ Done  "); uart_puts(jobs[i].cmd);
            if (sig) { uart_puts("  ("); uart_puts(signal_name(sig)); uart_puts(")"); }
            else if (code) { uart_puts("  (exit "); put_int(code); uart_puts(")"); }
            uart_puts("\r\n");
            if (st >= 0) (void)task_reap(jobs[i].pid);
            jobs[i].used = 0;
        }
    }
    (void)task_reap_orphans(SHELL_PID, 0);
}

static void print_final(int pid, int code)
{
    (void)pid;
    uart_puts("Userspace returned to the kernel. Exit code: ");
    put_int(code);
    uart_puts("\r\n");
}

static int wait_and_report(int pid, const char* cmd)
{
    int code = task_wait_foreground(pid, SHELL_PID);
    if (code == TASK_STOPPED)
    {
        Job* j = job_add(pid, cmd);
        if (j) { j->stopped = 1; uart_puts("\r\n["); put_int(j->id); uart_puts("]+ Stopped  "); uart_puts(cmd); uart_puts("\r\n"); }
        return TASK_STOPPED;
    }
    print_final(pid, code);
    script_set_status(code);
    return code;
}

void jobs_note_stopped(int pid, const char* cmd)
{
    Job* j = job_add(pid, cmd);
    if (j) j->stopped = 1;
}

int jobs_run_foreground(const char* path, const char* args, int in_fd, int out_fd)
{
    int pid = task_spawn(path, args, SHELL_PID, 0, in_fd, out_fd);
    if (pid < 0) return pid;
    return wait_and_report(pid, path);
}

static int run_pipeline(char* line)
{
    /* split on '|' */
    char* stage[6]; int n = 0;
    char* s = line;
    stage[n++] = s;
    for (; *s; ++s)
        if (*s == '|') { *s = 0; if (n < 6) stage[n++] = s + 1; else { uart_puts("pipeline: too many stages\r\n"); return 1; } }
    if (n < 2) return 0;

    char paths[6][96]; char* args[6]; int builtin_src = 0;
    char src_text[160]; src_text[0] = 0; int src_is_file = 0;
    for (int i = 0; i < n; ++i)
    {
        char* prog; char* a;
        split_stage(stage[i], &prog, &a);
        if (i == 0 && (str_equal(prog, "echo") || str_equal(prog, "cat")))
        {
            builtin_src = 1; src_is_file = str_equal(prog, "cat");
            str_copy(src_text, a, sizeof(src_text));
            paths[i][0] = 0; args[i] = a;
            continue;
        }
        if (!jobs_resolve_program(prog, paths[i], sizeof(paths[i]))) return 0;  /* not an ELF pipeline: legacy handler */
        args[i] = a;
    }

    int kp = process_pid_at(0); if (kp < 0) kp = 0;
    (void)fd_set_process(kp);
    int pipes[5][2];
    int stages_n = n;
    for (int i = 0; i < stages_n - 1; ++i)
        if (fd_pipe(pipes[i]) != 0) { uart_puts("pipeline: cannot create pipe\r\n"); return 1; }

    if (builtin_src)
    {
        uint8_t data[1024]; uint64_t len = 0;
        if (src_is_file)
        {
            uint64_t sz = 0;
            static uint8_t filebuf[FD_PIPE_CAPACITY];
            if (fs_read_file(src_text, vfs_cwd(), filebuf, sizeof(filebuf), &sz) != 0) { uart_puts("pipeline: cannot read source\r\n"); return 1; }
            (void)fd_write(pipes[0][1], filebuf, sz > FD_PIPE_CAPACITY ? FD_PIPE_CAPACITY : sz);
        }
        else
        {
            len = (uint64_t)str_len(src_text);
            for (uint64_t k = 0; k < len; ++k) data[k] = (uint8_t)src_text[k];
            data[len++] = '\n';
            (void)fd_write(pipes[0][1], data, len);
        }
    }

    int pids[6]; int started = 0;
    for (int i = builtin_src ? 1 : 0; i < stages_n; ++i)
    {
        int in_fd = i > 0 ? pipes[i - 1][0] : -1;
        int out_fd = i < stages_n - 1 ? pipes[i][1] : -1;
        int pid = task_spawn(paths[i], args[i], SHELL_PID, 0, in_fd, out_fd);
        if (pid < 0)
        {
            uart_puts("pipeline: failed to start "); uart_puts(paths[i]); uart_puts("\r\n");
            for (int k = 0; k < started; ++k) { (void)task_kill(pids[k], SIG_KILL); }
            break;
        }
        pids[started++] = pid;
    }
    (void)fd_set_process(kp);
    for (int i = 0; i < stages_n - 1; ++i) { (void)fd_close(pipes[i][0]); (void)fd_close(pipes[i][1]); }
    if (started == 0) return 1;

    int last = pids[started - 1];
    int code = task_wait_foreground(last, SHELL_PID);
    for (int k = 0; k < started - 1; ++k)
        (void)task_wait(SHELL_PID, pids[k], 0, 0);
    if (code != TASK_STOPPED) print_final(last, code);
    return 1;
}

static int parse_sig(const char* s)
{
    if (!s || s[0] != '-') return -1;
    ++s;
    int v = 0; int any = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); ++s; any = 1; }
    if (any && !*s) return v;
    if (str_equal(s, "INT")) return SIG_INT; if (str_equal(s, "KILL")) return SIG_KILL;
    if (str_equal(s, "TERM")) return SIG_TERM; if (str_equal(s, "STOP")) return SIG_STOP;
    if (str_equal(s, "CONT")) return SIG_CONT; if (str_equal(s, "HUP")) return SIG_HUP;
    if (str_equal(s, "USR1")) return SIG_USR1; if (str_equal(s, "USR2")) return SIG_USR2;
    if (str_equal(s, "TSTP")) return SIG_TSTP;
    return -2;
}

static void builtin_jobs()
{
    int any = 0;
    for (int i = 0; i < JOBS_MAX; ++i)
        if (jobs[i].used)
        {
            any = 1;
            uart_puts("["); put_int(jobs[i].id); uart_puts("]  ");
            uart_puts(process_is_stopped(jobs[i].pid) ? "Stopped  " : "Running  ");
            uart_puts("pid "); put_int(jobs[i].pid); uart_puts("  "); uart_puts(jobs[i].cmd); uart_puts("\r\n");
        }
    if (!any) uart_puts("no jobs\r\n");
}

int jobs_handle_line(const char* input)
{
    if (!input) return 0;
    char line[512];
    str_copy(line, input, sizeof(line));
    int len = str_len(line);
    while (len > 0 && line[len - 1] == ' ') line[--len] = 0;
    if (len == 0) return 0;

    /* built-ins: jobs fg bg kill wait */
    char first[16]; int fi = 0;
    while (line[fi] && line[fi] != ' ' && fi < 15) { first[fi] = line[fi]; ++fi; }
    first[fi] = 0;
    const char* rest = line + fi; while (*rest == ' ') ++rest;

    if (str_equal(first, "jobs")) { builtin_jobs(); return 1; }
    if (str_equal(first, "fg") || str_equal(first, "bg"))
    {
        Job* j = job_find(rest);
        if (!j) { uart_puts(first); uart_puts(": no such job\r\n"); return 1; }
        (void)task_kill(j->pid, SIG_CONT);
        j->stopped = 0;
        if (str_equal(first, "bg")) { (void)process_set_background(j->pid, 1); uart_puts("["); put_int(j->id); uart_puts("]+ "); uart_puts(j->cmd); uart_puts(" &\r\n"); return 1; }
        (void)process_set_background(j->pid, 0);
        uart_puts(j->cmd); uart_puts("\r\n");
        int pid = j->pid; char cmd[48]; str_copy(cmd, j->cmd, sizeof(cmd)); j->used = 0;
        (void)wait_and_report(pid, cmd);
        return 1;
    }
    if (str_equal(first, "kill"))
    {
        int sig = SIG_TERM; const char* arg = rest;
        char tok[16]; int ti = 0;
        while (arg[ti] && arg[ti] != ' ' && ti < 15) { tok[ti] = arg[ti]; ++ti; } tok[ti] = 0;
        if (tok[0] == '-')
        {
            sig = parse_sig(tok);
            if (sig < 0) { uart_puts("kill: unknown signal\r\n"); return 1; }
            arg += ti; while (*arg == ' ') ++arg;
        }
        if (!*arg) { uart_puts("kill: usage: kill [-SIGNAL] <pid|%job>\r\n"); return 1; }
        int pid = -1;
        if (arg[0] == '%') { Job* j = job_find(arg); if (j) pid = j->pid; }
        else { Job* j = job_find(arg); pid = j ? j->pid : 0; if (!j) { int v = 0; const char* q = arg; while (*q >= '0' && *q <= '9') v = v * 10 + (*q++ - '0'); pid = *q ? -1 : v; } }
        if (pid < 0) { uart_puts("kill: no such process\r\n"); return 1; }
        if (!security_is_root() && process_uid(pid) == 0 && process_is_user(pid)) { uart_puts("kill: permission denied\r\n"); return 1; }
        int rc = task_kill(pid, sig);
        if (rc == -1) uart_puts("kill: pid 0/1 are protected\r\n");
        else if (rc != 0) uart_puts("kill: no such process\r\n");
        return 1;
    }
    if (str_equal(first, "wait"))
    {
        if (*rest)
        {
            Job* j = job_find(rest);
            int pid = j ? j->pid : -1;
            if (!j) { int v = 0; const char* q = rest; while (*q >= '0' && *q <= '9') v = v * 10 + (*q++ - '0'); pid = *q ? -1 : v; }
            if (pid < 0 || !process_is_child_of(SHELL_PID, pid)) { uart_puts("wait: not a child of this shell\r\n"); return 1; }
            int st = 0; (void)task_wait(SHELL_PID, pid, &st, 0);
            if (j) j->used = 0;
            uart_puts("wait: pid "); put_int(pid); uart_puts(" done, status "); put_int(st); uart_puts("\r\n");
        }
        else
        {
            int st;
            while (process_has_children(SHELL_PID))
            {
                int z = task_wait(SHELL_PID, -1, &st, 0);
                if (z <= 0) break;
                for (int i = 0; i < JOBS_MAX; ++i) if (jobs[i].used && jobs[i].pid == z) jobs[i].used = 0;
                uart_puts("wait: pid "); put_int(z); uart_puts(" done, status "); put_int(st); uart_puts("\r\n");
            }
        }
        return 1;
    }

    /* pipeline of programs */
    for (int i = 0; line[i]; ++i)
        if (line[i] == '|')
        {
            char tmp[512]; str_copy(tmp, line, sizeof(tmp));
            return run_pipeline(tmp);
        }

    /* trailing & -> background program */
    if (line[len - 1] == '&')
    {
        line[--len] = 0;
        while (len > 0 && line[len - 1] == ' ') line[--len] = 0;
        char* prog; char* args; char copy[512]; str_copy(copy, line, sizeof(copy));
        split_stage(copy, &prog, &args);
        char path[96];
        if (str_equal(prog, "exec")) { char* a2; char* p2; split_stage(args, &p2, &a2); prog = p2; args = a2; }
        if (!jobs_resolve_program(prog, path, sizeof(path))) return 0;  /* builtin: run normally */
        int pid = task_spawn(path, args, SHELL_PID, 1, -1, -1);
        if (pid < 0) { uart_puts("exec: failed to start "); uart_puts(path); uart_puts("\r\n"); return 1; }
        Job* j = job_add(pid, line);
        uart_puts("["); put_int(j ? j->id : 0); uart_puts("] "); put_int(pid); uart_puts("\r\n");
        return 1;
    }
    return 0;
}
