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

#include "fd.h"
#include "fs.h"
#include "uart.h"
#include "netstack.h"
#include "tty.h"
#include "timer.h"
#include "process.h"

#define FD_KIND_NONE 0
#define FD_KIND_STDIN 1
#define FD_KIND_STDOUT 2
#define FD_KIND_STDERR 3
#define FD_KIND_FILE 4
#define FD_KIND_PIPE_READ 5
#define FD_KIND_PIPE_WRITE 6
#define FD_KIND_SOCK_TCP 7
#define FD_KIND_SOCK_UDP 8
#define FD_KIND_EVENT 9
#define FD_KIND_UNIX 10
#define FD_KIND_TIMER 11
#define FD_KIND_SIGNAL 12
#define FD_KIND_INOTIFY 13

#define POLL_IN 1
#define POLL_OUT 4
#define POLL_ERR 8
#define POLL_HUP 16
#define POLL_NVAL 32
#define SOCK_OWNER_BASE 0x20000

#define FD_SEEK_SET 0
#define FD_SEEK_CUR 1
#define FD_SEEK_END 2

#define FD_PROCESS_TABLES 48
#define FD_OPEN_FILES 128
#define FD_PIPE_MAX 32
#define FD_PIPE_CAPACITY 4096

struct FdEntry
{
    int used;
    int object;
    int cloexec;     /* FD_CLOEXEC: closed by exec */
};

struct FdTable
{
    int used;
    int owner_pid;
    FdEntry entries[FD_MAX_DESCRIPTORS];
};

struct OpenFile
{
    int used;
    int permanent;
    int kind;
    int refs;
    uint32_t flags;
    int cwd;
    uint64_t offset;
    int pipe_id;
    int aux;            /* net handle / event index / unix socket index */
    int aux2;           /* bound port (TCP), connected flag (UDP) */
    int owner;          /* net owner key for socket objects */
    uint32_t peer_ip;   /* connected UDP peer */
    uint16_t peer_port;
    char path[128];
};

struct PipeState
{
    int used;
    int readers;
    int writers;
    uint32_t read_pos;
    uint32_t write_pos;
    uint32_t size;
    uint32_t gen;                       /* bumped on every write / last-writer close (epoll EPOLLET) */
    uint8_t data[FD_PIPE_CAPACITY];
};


/* ---- timerfd ---- */
#define TF_MAX 16
struct TimerFd { int used; uint64_t interval_ms; uint64_t next_ms; uint64_t expirations; uint32_t gen; };
static TimerFd timerfds[TF_MAX];

/* ---- signalfd: signals of a mask are read as 128-byte signalfd_siginfo records instead of being delivered ---- */
#define SF_MAX 8
struct SigFd { int used; uint32_t mask; };
static SigFd sigfds[SF_MAX];

/* ---- inotify: change events for watched files and directories (fed by fs_event_hook and by the descriptor layer) ---- */
#define IN_INST_MAX 4
#define IN_WATCH_MAX 16
#define IN_QUEUE_MAX 64
#define IN_NAME_MAX 64
struct InoWatch { int wd; int node; uint32_t mask; };            /* wd 0 = free slot */
struct InoEv { int wd; uint32_t mask; uint32_t cookie; int len; char name[IN_NAME_MAX]; };
struct Ino { int used; int next_wd; InoWatch w[IN_WATCH_MAX]; InoEv q[IN_QUEUE_MAX]; int qh, qn; int overflow; uint32_t gen; };
static Ino inos[IN_INST_MAX];
static int ino_active;
static void ino_file_event(const char* path, int cwd, uint32_t mask);

/* ---- flock (advisory, per open file description) ---- */
#define FL_MAX 64
struct FLock { int used; int node; int ex; int obj; };
static FLock flocks[FL_MAX];

/* ---- eventfd ---- */
#define EV_MAX 32
struct EventState { int used; int semaphore; uint64_t counter; uint32_t gen; };
static EventState events[EV_MAX];

/* ---- AF_UNIX sockets (stream + datagram), socketpair, named listeners, SCM_RIGHTS ---- */
#define UX_MAX 32
#define UX_RING 8192
#define UX_BACKLOG 8
#define UX_FDQ 8
#define UX_DGRAM_MAX 2048
struct UnixSock
{
    int used;
    int type;            /* 1 stream, 2 datagram */
    int state;           /* 0 fresh, 1 listening, 2 connected */
    int peer;            /* index of connected peer or -1 */
    int peer_closed;
    int wr_shut;
    int attached;        /* has an OpenFile (false for queued, not-yet-accepted server ends) */
    char path[108];
    uint32_t rd, wr, size;
    uint64_t tot_w, tot_r;
    uint32_t gen;        /* bumped on data arrival, EOF and new connections (epoll EPOLLET) */
    int backlog[UX_BACKLOG]; int nback;
    int fdq_obj[UX_FDQ]; uint64_t fdq_end[UX_FDQ]; int nfdq;
    uint8_t ring[UX_RING];
};
static UnixSock usocks[UX_MAX];
static FdTable tables[FD_PROCESS_TABLES];
static OpenFile open_files[FD_OPEN_FILES];
static PipeState pipes[FD_PIPE_MAX];
static int active_table = -1;
static uint8_t file_buffer[FD_FILE_MAX];

static int string_length(const char* s)
{
    int n = 0;
    if (!s) return 0;
    while (s[n]) ++n;
    return n;
}

static void copy_string(char* dst, const char* src, int cap)
{
    if (!dst || cap <= 0) return;
    int i = 0;
    if (src)
        while (src[i] && i + 1 < cap) { dst[i] = src[i]; ++i; }
    dst[i] = '\0';
}

static void print_u64(uint64_t value)
{
    char digits[24];
    int n = 0;
    if (value == 0) digits[n++] = '0';
    while (value && n < (int)sizeof(digits))
    {
        digits[n++] = (char)('0' + (value % 10ULL));
        value /= 10ULL;
    }
    while (n) uart_putc(digits[--n]);
}

static uint32_t access_mode(uint32_t flags) { return flags & 3U; }
static int can_read(uint32_t flags)
{
    uint32_t mode = access_mode(flags);
    return mode == FD_O_RDONLY || mode == FD_O_RDWR;
}
static int can_write(uint32_t flags)
{
    uint32_t mode = access_mode(flags);
    return mode == FD_O_WRONLY || mode == FD_O_RDWR;
}

static int valid_table(int index)
{
    return index >= 0 && index < FD_PROCESS_TABLES && tables[index].used;
}

static int find_table(int pid)
{
    for (int i = 0; i < FD_PROCESS_TABLES; ++i)
        if (tables[i].used && tables[i].owner_pid == pid) return i;
    return -1;
}

static void clear_descriptor(FdTable* table, int descriptor)
{
    if (!table || descriptor < 0 || descriptor >= FD_MAX_DESCRIPTORS) return;
    table->entries[descriptor].used = 0;
    table->entries[descriptor].object = -1;
    table->entries[descriptor].cloexec = 0;
}

static void install_standard_descriptors(FdTable* table)
{
    if (!table) return;
    for (int i = 0; i < 3; ++i)
    {
        table->entries[i].used = 1;
        table->entries[i].object = i;
    }
}

static int create_table(int pid)
{
    if (pid < 0) return -1;
    int existing = find_table(pid);
    if (existing >= 0) return existing;

    for (int i = 0; i < FD_PROCESS_TABLES; ++i)
    {
        if (tables[i].used) continue;
        tables[i].used = 1;
        tables[i].owner_pid = pid;
        for (int d = 0; d < FD_MAX_DESCRIPTORS; ++d)
            clear_descriptor(&tables[i], d);
        install_standard_descriptors(&tables[i]);
        return i;
    }
    return -1;
}

static FdTable* current_table()
{
    return valid_table(active_table) ? &tables[active_table] : 0;
}

static int descriptor_valid_in(FdTable* table, int descriptor)
{
    if (!table || descriptor < 0 || descriptor >= FD_MAX_DESCRIPTORS ||
        !table->entries[descriptor].used) return 0;
    int object = table->entries[descriptor].object;
    return object >= 0 && object < FD_OPEN_FILES && open_files[object].used;
}

/* RLIMIT_NOFILE of the process that owns the table (enforced for allocation of new descriptors) */
static int fd_limit_of(FdTable* table)
{
    uint64_t lim = table ? process_rlimit_cur(table->owner_pid, PROC_RLIMIT_NOFILE) : FD_MAX_DESCRIPTORS;
    return lim > FD_MAX_DESCRIPTORS ? FD_MAX_DESCRIPTORS : (int)lim;
}

static int free_descriptor(FdTable* table, int minimum)
{
    if (!table) return -1;
    int limit = fd_limit_of(table);
    for (int i = minimum; i < limit; ++i)
        if (!table->entries[i].used) return i;
    return -1;
}

static int free_object()
{
    for (int i = 3; i < FD_OPEN_FILES; ++i)
        if (!open_files[i].used) return i;
    return -1;
}

static int free_pipe()
{
    for (int i = 0; i < FD_PIPE_MAX; ++i)
        if (!pipes[i].used) return i;
    return -1;
}

static void clear_object(int object)
{
    if (object < 3 || object >= FD_OPEN_FILES) return;
    open_files[object].used = 0;
    open_files[object].permanent = 0;
    open_files[object].kind = FD_KIND_NONE;
    open_files[object].refs = 0;
    open_files[object].flags = 0;
    open_files[object].cwd = -1;
    open_files[object].offset = 0;
    open_files[object].pipe_id = -1;
    open_files[object].aux = -1;
    open_files[object].aux2 = 0;
    open_files[object].owner = -1;
    open_files[object].peer_ip = 0;
    open_files[object].peer_port = 0;
    open_files[object].path[0] = '\0';
}

static void maybe_destroy_pipe(int pipe_id)
{
    if (pipe_id < 0 || pipe_id >= FD_PIPE_MAX || !pipes[pipe_id].used) return;
    if (pipes[pipe_id].readers == 0 && pipes[pipe_id].writers == 0)
    {
        pipes[pipe_id].used = 0;
        pipes[pipe_id].read_pos = 0;
        pipes[pipe_id].write_pos = 0;
        pipes[pipe_id].size = 0;
    }
}


static void release_object(int object);

static void ux_drop_fdq(UnixSock* u)
{
    int objs[UX_FDQ]; int n = u->nfdq;
    for (int i = 0; i < n; ++i) objs[i] = u->fdq_obj[i];
    u->nfdq = 0;
    for (int i = 0; i < n; ++i) release_object(objs[i]);
}

static void ux_free(int idx)
{
    if (idx < 0 || idx >= UX_MAX || !usocks[idx].used) return;
    UnixSock* u = &usocks[idx];
    int peer = u->peer;
    if (peer >= 0 && usocks[peer].used) { usocks[peer].peer = -1; usocks[peer].peer_closed = 1; ++usocks[peer].gen; }
    if (u->state == 1)
    {
        for (int i = 0; i < u->nback; ++i)
        {
            int srv = u->backlog[i];
            if (srv < 0 || !usocks[srv].used) continue;
            int cl = usocks[srv].peer;
            if (cl >= 0 && usocks[cl].used) { usocks[cl].peer = -1; usocks[cl].peer_closed = 1; ++usocks[cl].gen; }
            usocks[srv].used = 0;
        }
        u->nback = 0;
    }
    u->used = 0;
    ux_drop_fdq(u);
}

static int ux_alloc(int type)
{
    for (int i = 0; i < UX_MAX; ++i)
    {
        if (usocks[i].used) continue;
        UnixSock* u = &usocks[i];
        u->used = 1; u->type = type; u->state = 0; u->peer = -1; u->peer_closed = 0; u->wr_shut = 0;
        u->attached = 0; u->path[0] = 0; u->rd = u->wr = u->size = 0; u->tot_w = u->tot_r = 0;
        u->nback = 0; u->nfdq = 0;
        return i;
    }
    return -1;
}

static void release_object(int object)
{
    if (object < 0 || object >= FD_OPEN_FILES || !open_files[object].used) return;
    OpenFile* file = &open_files[object];
    if (file->permanent) return;
    if (file->refs > 0) --file->refs;
    if (file->refs > 0) return;

    int pipe_id = file->pipe_id;
    if (pipe_id >= 0 && pipe_id < FD_PIPE_MAX && pipes[pipe_id].used)
    {
        if (file->kind == FD_KIND_PIPE_READ && pipes[pipe_id].readers > 0)
            --pipes[pipe_id].readers;
        if (file->kind == FD_KIND_PIPE_WRITE && pipes[pipe_id].writers > 0)
            { --pipes[pipe_id].writers; ++pipes[pipe_id].gen; }
    }
    if (file->kind == FD_KIND_SOCK_TCP && file->aux > 0) (void)net_tcp_close(file->owner, file->aux);
    else if (file->kind == FD_KIND_SOCK_UDP && file->aux > 0) (void)net_udp_close(file->owner, file->aux);
    else if (file->kind == FD_KIND_EVENT && file->aux >= 0 && file->aux < EV_MAX) events[file->aux].used = 0;
    else if (file->kind == FD_KIND_TIMER && file->aux >= 0 && file->aux < TF_MAX) timerfds[file->aux].used = 0;
    else if (file->kind == FD_KIND_SIGNAL && file->aux >= 0 && file->aux < SF_MAX) sigfds[file->aux].used = 0;
    else if (file->kind == FD_KIND_INOTIFY && file->aux >= 0 && file->aux < IN_INST_MAX) { inos[file->aux].used = 0; if (--ino_active <= 0) { ino_active = 0; fs_event_hook = 0; } }
    else if (file->kind == FD_KIND_FILE)
    {
        for (int i = 0; i < FL_MAX; ++i) { if (flocks[i].used && flocks[i].obj == object) flocks[i].used = 0; }
        if (ino_active) ino_file_event(file->path, file->cwd, can_write(file->flags) ? 0x8U : 0x10U);   /* IN_CLOSE_WRITE / IN_CLOSE_NOWRITE */
    }
    if (file->kind == FD_KIND_UNIX) { int ux = file->aux; clear_object(object); ux_free(ux); maybe_destroy_pipe(pipe_id); return; }
    clear_object(object);
    maybe_destroy_pipe(pipe_id);
}

static void close_entry(FdTable* table, int descriptor)
{
    if (!descriptor_valid_in(table, descriptor))
    {
        clear_descriptor(table, descriptor);
        return;
    }
    int object = table->entries[descriptor].object;
    clear_descriptor(table, descriptor);
    release_object(object);
}

static int attach_object(FdTable* table, int object, int minimum)
{
    if (!table || object < 0 || object >= FD_OPEN_FILES || !open_files[object].used)
        return -1;
    int descriptor = free_descriptor(table, minimum);
    if (descriptor < 0) return -24;
    table->entries[descriptor].used = 1;
    table->entries[descriptor].object = object;
    if (!open_files[object].permanent) ++open_files[object].refs;
    return descriptor;
}

void fd_init()
{
    for (int i = 0; i < FD_PROCESS_TABLES; ++i)
    {
        tables[i].used = 0;
        tables[i].owner_pid = -1;
        for (int d = 0; d < FD_MAX_DESCRIPTORS; ++d)
        {
            tables[i].entries[d].used = 0;
            tables[i].entries[d].object = -1;
        }
    }
    for (int i = 0; i < FD_OPEN_FILES; ++i) clear_object(i);
    for (int i = 0; i < EV_MAX; ++i) events[i].used = 0;
    for (int i = 0; i < TF_MAX; ++i) timerfds[i].used = 0;
    for (int i = 0; i < SF_MAX; ++i) sigfds[i].used = 0;
    for (int i = 0; i < IN_INST_MAX; ++i) inos[i].used = 0;
    ino_active = 0; fs_event_hook = 0;
    for (int i = 0; i < FL_MAX; ++i) flocks[i].used = 0;
    for (int i = 0; i < UX_MAX; ++i) usocks[i].used = 0;
    for (int i = 0; i < FD_PIPE_MAX; ++i)
    {
        pipes[i].used = 0;
        pipes[i].readers = pipes[i].writers = 0;
        pipes[i].read_pos = pipes[i].write_pos = pipes[i].size = 0;
    }

    /* Standard streams are immortal shared endpoints; each process table has
       its own descriptor slots referring to these stateless objects. */
    for (int i = 0; i < 3; ++i)
    {
        open_files[i].used = 1;
        open_files[i].permanent = 1;
        open_files[i].kind = i == 0 ? FD_KIND_STDIN : (i == 1 ? FD_KIND_STDOUT : FD_KIND_STDERR);
        open_files[i].refs = 0;
        open_files[i].flags = i == 0 ? FD_O_RDONLY : FD_O_WRONLY;
        open_files[i].cwd = -1;
        open_files[i].offset = 0;
        open_files[i].pipe_id = -1;
        open_files[i].path[0] = '\0';
    }
    active_table = create_table(0);
}

int fd_set_process(int pid)
{
    if (pid < 0) return -1;
    int index = find_table(pid);
    if (index < 0) index = create_table(pid);
    if (index < 0) return -24;
    active_table = index;
    return 0;
}

int fd_current_process()
{
    FdTable* table = current_table();
    return table ? table->owner_pid : -1;
}

void fd_close_all()
{
    FdTable* table = current_table();
    if (!table) return;
    for (int i = 0; i < FD_MAX_DESCRIPTORS; ++i)
        close_entry(table, i);
    install_standard_descriptors(table);
}

int fd_close_all_for(int pid)
{
    if (fd_set_process(pid) != 0) return -24;
    fd_close_all();
    return 0;
}

void fd_release_process(int pid)
{
    if (pid == 0) return; /* Keep the kernel descriptor table permanent. */
    int index = find_table(pid);
    if (index < 0) return;
    FdTable* table = &tables[index];
    for (int i = 0; i < FD_MAX_DESCRIPTORS; ++i)
        close_entry(table, i);
    table->used = 0;
    table->owner_pid = -1;
    if (active_table == index)
    {
        active_table = find_table(0);
        if (active_table < 0) active_table = create_table(0);
    }
}

int fd_open(const char* path, uint32_t flags, int cwd)
{
    FdTable* table = current_table();
    if (!table) return -24;
    if (!path || !path[0] || string_length(path) >= (int)sizeof(open_files[0].path))
        return -22;
    const int want_cloexec = (flags & 0x1000U) != 0;   /* O_CLOEXEC */
    const int want_excl = (flags & 0x2000U) != 0;      /* O_EXCL (with O_CREAT: fail when the file exists) */
    flags &= ~0x3000U;
    if (want_excl && !(flags & FD_O_CREAT)) return -22;
    if ((flags & ~(FD_O_CREAT | FD_O_TRUNC | FD_O_APPEND | 3U)) != 0 ||
        access_mode(flags) == 3U)
        return -22;
    if ((flags & FD_O_TRUNC) && !can_write(flags)) return -22;

    int descriptor = free_descriptor(table, 3);
    if (descriptor < 0) return -24;
    int object = free_object();
    if (object < 0) return -24;

    int id = fs_resolve(path, cwd);
    if (id >= 0 && want_excl) return -17;
    if (id < 0)
    {
        if (!(flags & FD_O_CREAT)) return -2;
        id = fs_touch(path, cwd);
        if (id < 0) return -5;
    }
    if (fs_get_type(id) != FS_FILE) return -21;
    if (can_read(flags) && fs_check_access(id, FS_ACCESS_READ) != 0) return -13;
    if (can_write(flags) && fs_check_access(id, FS_ACCESS_WRITE) != 0) return -13;

    if (flags & FD_O_TRUNC)
    {
        static const uint8_t empty = 0;
        if (fs_write_data(path, cwd, &empty, 0) != 0) return -5;
    }

    OpenFile* file = &open_files[object];
    file->used = 1;
    file->permanent = 0;
    file->kind = FD_KIND_FILE;
    file->refs = 1;
    file->flags = flags;
    file->cwd = cwd;
    file->offset = 0;
    file->pipe_id = -1;
    copy_string(file->path, path, (int)sizeof(file->path));
    if (flags & FD_O_APPEND) file->offset = fs_file_size(id);

    table->entries[descriptor].used = 1;
    table->entries[descriptor].object = object;
    table->entries[descriptor].cloexec = want_cloexec;
    if (ino_active) ino_file_event(path, cwd, 0x20U);                           /* IN_OPEN */
    return descriptor;
}

int fd_close(int descriptor)
{
    FdTable* table = current_table();
    if (!descriptor_valid_in(table, descriptor)) return -9;
    close_entry(table, descriptor);
    return 0;
}

int fd_dup(int descriptor)
{
    FdTable* table = current_table();
    if (!descriptor_valid_in(table, descriptor)) return -9;
    return attach_object(table, table->entries[descriptor].object, 0);
}

int fd_dup2(int old_descriptor, int new_descriptor)
{
    FdTable* table = current_table();
    if (!descriptor_valid_in(table, old_descriptor)) return -9;
    if (new_descriptor < 0 || new_descriptor >= FD_MAX_DESCRIPTORS) return -9;
    if (old_descriptor == new_descriptor) return new_descriptor;
    if (new_descriptor >= fd_limit_of(table)) return -9;      /* EBADF: beyond RLIMIT_NOFILE */

    int object = table->entries[old_descriptor].object;
    if (table->entries[new_descriptor].used) close_entry(table, new_descriptor);
    table->entries[new_descriptor].used = 1;
    table->entries[new_descriptor].object = object;
    if (!open_files[object].permanent) ++open_files[object].refs;
    return new_descriptor;
}

int fd_pipe(int descriptors[2])
{
    FdTable* table = current_table();
    if (!table || !descriptors) return -14;
    int first = free_descriptor(table, 3);
    if (first < 0) return -24;
    int second = -1;
    for (int i = first + 1; i < fd_limit_of(table); ++i)
        if (!table->entries[i].used) { second = i; break; }
    if (second < 0) return -24;

    int pipe_id = free_pipe();
    if (pipe_id < 0) return -24;
    int read_object = free_object();
    if (read_object < 0) return -24;
    /* Reserve the second object distinctly before publishing descriptors. */
    open_files[read_object].used = 1;
    int write_object = free_object();
    open_files[read_object].used = 0;
    if (write_object < 0 || write_object == read_object) return -24;

    PipeState* pipe = &pipes[pipe_id];
    pipe->used = 1;
    pipe->readers = 1;
    pipe->writers = 1;
    pipe->read_pos = pipe->write_pos = pipe->size = 0;

    OpenFile* rd = &open_files[read_object];
    rd->used = 1; rd->permanent = 0; rd->kind = FD_KIND_PIPE_READ;
    rd->refs = 1; rd->flags = FD_O_RDONLY; rd->cwd = -1; rd->offset = 0;
    rd->pipe_id = pipe_id; rd->path[0] = '\0';
    OpenFile* wr = &open_files[write_object];
    wr->used = 1; wr->permanent = 0; wr->kind = FD_KIND_PIPE_WRITE;
    wr->refs = 1; wr->flags = FD_O_WRONLY; wr->cwd = -1; wr->offset = 0;
    wr->pipe_id = pipe_id; wr->path[0] = '\0';

    table->entries[first].used = 1;
    table->entries[first].object = read_object;
    table->entries[second].used = 1;
    table->entries[second].object = write_object;
    descriptors[0] = first;
    descriptors[1] = second;
    return 0;
}

static OpenFile* file_of(int descriptor);
static int new_object_fd(int kind, uint32_t flags, int aux, int owner, int cloexec);
#ifndef SOCK_NONBLOCK_FLAG
#define SOCK_TYPE_MASK 0xF
#define SOCK_NONBLOCK_FLAG 0x800
#define SOCK_CLOEXEC_FLAG 0x1000
#endif

static void tf_update(TimerFd* tf)
{
    if (!tf->next_ms) return;
    uint64_t now = timer_millis();
    if (now < tf->next_ms) return;
    if (tf->interval_ms)
    {
        uint64_t n = 1 + (now - tf->next_ms) / tf->interval_ms;
        tf->expirations += n; ++tf->gen;
        tf->next_ms += n * tf->interval_ms;
    }
    else { tf->expirations += 1; tf->next_ms = 0; ++tf->gen; }
}

int fd_timerfd_create(int flags)
{
    int idx = -1;
    for (int i = 0; i < TF_MAX; ++i) if (!timerfds[i].used) { idx = i; break; }
    if (idx < 0) return -24;
    timerfds[idx].used = 1; timerfds[idx].interval_ms = 0; timerfds[idx].next_ms = 0; timerfds[idx].expirations = 0;
    int d = new_object_fd(FD_KIND_TIMER, FD_O_RDWR | ((flags & SOCK_NONBLOCK_FLAG) ? FD_O_NONBLOCK : 0), idx, -1, (flags & SOCK_CLOEXEC_FLAG) != 0);
    if (d < 0) timerfds[idx].used = 0;
    return d;
}

/* first_ms == 0 disarms. old[0] = remaining ms, old[1] = interval ms (both may be NULL) */
int fd_timerfd_settime(int descriptor, uint64_t first_ms, uint64_t interval_ms, uint64_t* old)
{
    OpenFile* f = file_of(descriptor);
    if (!f || f->kind != FD_KIND_TIMER) return f ? -22 : -9;
    TimerFd* tf = &timerfds[f->aux];
    tf_update(tf);
    if (old) { uint64_t now = timer_millis(); old[0] = tf->next_ms && tf->next_ms > now ? tf->next_ms - now : 0; old[1] = tf->interval_ms; }
    tf->expirations = 0;
    tf->interval_ms = interval_ms;
    tf->next_ms = first_ms ? timer_millis() + first_ms : 0;
    return 0;
}

int fd_timerfd_gettime(int descriptor, uint64_t out[2])
{
    OpenFile* f = file_of(descriptor);
    if (!f || f->kind != FD_KIND_TIMER) return f ? -22 : -9;
    TimerFd* tf = &timerfds[f->aux];
    tf_update(tf);
    uint64_t now = timer_millis();
    out[0] = tf->next_ms && tf->next_ms > now ? tf->next_ms - now : 0; out[1] = tf->interval_ms;
    return 0;
}

/* fstat: regular files go through the filesystem; other kinds are synthesized (type 3 fifo, 4 socket, 5 tty, 6 anonymous) */
/* ---- signalfd ---- */
#define SIGKILL_BIT (1U << 9)
#define SIGSTOP_BIT (1U << 19)
int fd_signalfd(int descriptor, uint32_t mask, int flags)
{
    mask &= ~(SIGKILL_BIT | SIGSTOP_BIT | 1U);
    if (descriptor >= 0)
    {
        OpenFile* f = file_of(descriptor);
        if (!f) return -9;
        if (f->kind != FD_KIND_SIGNAL || f->aux < 0 || f->aux >= SF_MAX || !sigfds[f->aux].used) return -22;
        sigfds[f->aux].mask = mask;
        return descriptor;
    }
    int idx = -1;
    for (int i = 0; i < SF_MAX; ++i) if (!sigfds[i].used) { idx = i; break; }
    if (idx < 0) return -24;
    sigfds[idx].used = 1; sigfds[idx].mask = mask;
    int d = new_object_fd(FD_KIND_SIGNAL, FD_O_RDONLY | ((flags & SOCK_NONBLOCK_FLAG) ? FD_O_NONBLOCK : 0), idx, -1, (flags & SOCK_CLOEXEC_FLAG) != 0);
    if (d < 0) sigfds[idx].used = 0;
    return d;
}

/* one 128-byte struct signalfd_siginfo per pending signal of the mask; -EAGAIN when none (the syscall layer blocks) */
static int64_t signalfd_read(OpenFile* entry, void* buffer, uint64_t length)
{
    if (entry->aux < 0 || entry->aux >= SF_MAX || !sigfds[entry->aux].used) return -9;
    if (length < 128) return -22;
    FdTable* t = current_table();
    if (!t) return -9;
    int pid = t->owner_pid;
    uint8_t* out = (uint8_t*)buffer; uint64_t done = 0;
    while (done + 128 <= length)
    {
        int sig = process_sig_take_set(pid, sigfds[entry->aux].mask);
        if (sig <= 0) break;
        int sender = 0, code = 0; uint64_t value = 0;
        (void)process_sig_info_get(pid, sig, &sender, &code, &value);
        uint8_t* r = out + done; for (int i = 0; i < 128; ++i) r[i] = 0;
        *(uint32_t*)(r + 0) = (uint32_t)sig;                 /* ssi_signo */
        *(int32_t*)(r + 8) = code;                           /* ssi_code */
        *(uint32_t*)(r + 12) = (uint32_t)sender;             /* ssi_pid */
        *(uint32_t*)(r + 16) = process_uid(sender > 0 ? sender : pid);   /* ssi_uid */
        *(uint64_t*)(r + 48) = value;                        /* ssi_ptr (sigqueue value) */
        done += 128;
    }
    return done ? (int64_t)done : -11;
}

/* ---- inotify ---- */
#define IN_MODIFY_B 0x2U
#define IN_DELETE_SELF_B 0x400U
#define IN_MOVE_SELF_B 0x800U
#define IN_Q_OVERFLOW_B 0x4000U
#define IN_IGNORED_B 0x8000U
#define IN_ISDIR_B 0x40000000U
#define IN_ONESHOT_B 0x80000000U
#define IN_MASK_ADD_B 0x20000000U
#define IN_ONLYDIR_B 0x01000000U
#define IN_DONT_FOLLOW_B 0x02000000U
#define IN_ALL_EVENTS_B 0x00000FFFU

static void ino_queue(Ino* in, int wd, uint32_t mask, uint32_t cookie, const char* name)
{
    int len = 0; if (name) while (name[len] && len < IN_NAME_MAX - 1) ++len;
    if (in->qn > 0)
    {   /* identical to the newest queued event: coalesce (as Linux does) */
        InoEv* last = &in->q[(in->qh + in->qn - 1) % IN_QUEUE_MAX];
        if (last->wd == wd && last->mask == mask && last->cookie == cookie && last->len == len)
        {
            int same = 1; for (int i = 0; i < len; ++i) if (last->name[i] != name[i]) { same = 0; break; }
            if (same) return;
        }
    }
    if (in->qn >= IN_QUEUE_MAX - 1)
    {
        if (!in->overflow) { in->overflow = 1; InoEv* e = &in->q[(in->qh + in->qn) % IN_QUEUE_MAX]; e->wd = -1; e->mask = IN_Q_OVERFLOW_B; e->cookie = 0; e->len = 0; e->name[0] = 0; ++in->qn; ++in->gen; }
        return;
    }
    InoEv* e = &in->q[(in->qh + in->qn) % IN_QUEUE_MAX];
    e->wd = wd; e->mask = mask; e->cookie = cookie; e->len = len;
    for (int i = 0; i < len; ++i) e->name[i] = name[i];
    e->name[len] = 0;
    ++in->qn; ++in->gen;
}

/* called for every change: dir = directory whose entry changed, node = the node, name = entry name */
static void ino_dispatch(int dir, int node, const char* name, uint32_t mask, uint32_t cookie)
{
    uint32_t ev = mask & ~IN_ISDIR_B;
    int self_only = (ev & (IN_DELETE_SELF_B | IN_MOVE_SELF_B)) != 0;
    int child_only = (ev & (0x100U | 0x200U | 0x40U | 0x80U)) != 0;       /* CREATE DELETE MOVED_FROM MOVED_TO */
    for (int k = 0; k < IN_INST_MAX; ++k)
    {
        Ino* in = &inos[k]; if (!in->used) continue;
        for (int i = 0; i < IN_WATCH_MAX; ++i)
        {
            InoWatch* w = &in->w[i]; if (!w->wd || !(w->mask & ev)) continue;
            if (!child_only && w->node == node) ino_queue(in, w->wd, mask, cookie, "");          /* the watched object itself */
            else if (!self_only && dir >= 0 && w->node == dir) ino_queue(in, w->wd, mask, cookie, name);                      /* an entry of the watched directory */
            else continue;
            if (ev & IN_DELETE_SELF_B) { ino_queue(in, w->wd, IN_IGNORED_B, 0, ""); w->wd = 0; continue; }
            if (w->mask & IN_ONESHOT_B) { ino_queue(in, w->wd, IN_IGNORED_B, 0, ""); w->wd = 0; }
        }
    }
}
static void ino_hook(int dir, int node, const char* name, uint32_t mask, uint32_t cookie) { ino_dispatch(dir, node, name, mask, cookie); }

/* OPEN / ACCESS / CLOSE_* come from the descriptor layer */
static void ino_file_event(const char* path, int cwd, uint32_t mask)
{
    if (!ino_active) return;
    int id = fs_resolve(path, cwd);
    if (id < 0) return;
    ino_dispatch(fs_get_parent(id), id, fs_get_name(id), mask, 0);
}

int fd_inotify_init(int flags)
{
    int idx = -1;
    for (int i = 0; i < IN_INST_MAX; ++i) if (!inos[i].used) { idx = i; break; }
    if (idx < 0) return -24;
    Ino* in = &inos[idx];
    in->used = 1; in->next_wd = 1; in->qh = in->qn = 0; in->overflow = 0; in->gen = 0;
    for (int i = 0; i < IN_WATCH_MAX; ++i) in->w[i].wd = 0;
    int d = new_object_fd(FD_KIND_INOTIFY, FD_O_RDONLY | ((flags & SOCK_NONBLOCK_FLAG) ? FD_O_NONBLOCK : 0), idx, -1, (flags & SOCK_CLOEXEC_FLAG) != 0);
    if (d < 0) { in->used = 0; return d; }
    ++ino_active; fs_event_hook = ino_hook;
    return d;
}

static Ino* ino_of(int descriptor)
{
    OpenFile* f = file_of(descriptor);
    if (!f || f->kind != FD_KIND_INOTIFY || f->aux < 0 || f->aux >= IN_INST_MAX || !inos[f->aux].used) return 0;
    return &inos[f->aux];
}

int fd_inotify_add_watch(int descriptor, const char* path, uint32_t mask, int cwd)
{
    Ino* in = ino_of(descriptor);
    if (!in) return file_of(descriptor) ? -22 : -9;
    if (!path || !path[0]) return -2;
    if ((mask & IN_ALL_EVENTS_B) == 0) return -22;
    int id = (mask & IN_DONT_FOLLOW_B) ? fs_resolve_nofollow(path, cwd) : fs_resolve(path, cwd);
    if (id < 0) return -2;
    if ((mask & IN_ONLYDIR_B) && fs_get_type(id) != FS_DIR) return -20;
    if (fs_check_access(id, FS_ACCESS_READ) != 0) return -13;
    mask &= ~(IN_DONT_FOLLOW_B | IN_ONLYDIR_B);
    for (int i = 0; i < IN_WATCH_MAX; ++i)
        if (in->w[i].wd && in->w[i].node == id)
        {
            in->w[i].mask = (mask & IN_MASK_ADD_B) ? (in->w[i].mask | (mask & ~IN_MASK_ADD_B)) : (mask & ~IN_MASK_ADD_B);
            return in->w[i].wd;
        }
    for (int i = 0; i < IN_WATCH_MAX; ++i)
        if (!in->w[i].wd) { in->w[i].wd = in->next_wd++; in->w[i].node = id; in->w[i].mask = mask & ~IN_MASK_ADD_B; return in->w[i].wd; }
    return -28;                                   /* ENOSPC: too many watches */
}

int fd_inotify_rm_watch(int descriptor, int wd)
{
    Ino* in = ino_of(descriptor);
    if (!in) return file_of(descriptor) ? -22 : -9;
    for (int i = 0; i < IN_WATCH_MAX; ++i)
        if (in->w[i].wd == wd && wd > 0) { in->w[i].wd = 0; ino_queue(in, wd, IN_IGNORED_B, 0, ""); return 0; }
    return -22;
}

static int64_t inotify_read(OpenFile* entry, void* buffer, uint64_t length)
{
    Ino* in = (entry->aux >= 0 && entry->aux < IN_INST_MAX && inos[entry->aux].used) ? &inos[entry->aux] : 0;
    if (!in) return -9;
    if (in->qn == 0) return -11;
    uint8_t* out = (uint8_t*)buffer; uint64_t done = 0;
    while (in->qn > 0)
    {
        InoEv* e = &in->q[in->qh];
        uint32_t nl = e->len ? (uint32_t)((e->len + 1 + 3) & ~3) : 0;
        uint64_t need = 16U + nl;
        if (done + need > length) { if (done == 0) return -22; break; }
        uint8_t* r = out + done;
        *(int32_t*)(r + 0) = e->wd; *(uint32_t*)(r + 4) = e->mask; *(uint32_t*)(r + 8) = e->cookie; *(uint32_t*)(r + 12) = nl;
        for (uint32_t i = 0; i < nl; ++i) r[16 + i] = (int)i < e->len ? (uint8_t)e->name[i] : 0;
        done += need;
        in->qh = (in->qh + 1) % IN_QUEUE_MAX; --in->qn;
        if (e->mask == IN_Q_OVERFLOW_B) in->overflow = 0;
    }
    return (int64_t)done;
}

int fd_fstat(int descriptor, FsStat* out)
{
    OpenFile* f = file_of(descriptor);
    if (!f) return -9;
    uint8_t* z = (uint8_t*)out; for (uint64_t i = 0; i < sizeof(FsStat); ++i) z[i] = 0;
    if (f->kind == FD_KIND_FILE) return fs_stat_path(f->path, f->cwd, out);
    out->mode = 0600;
    switch (f->kind)
    {
        case FD_KIND_STDIN: case FD_KIND_STDOUT: case FD_KIND_STDERR: out->type = 5; out->mode = 0620; break;
        case FD_KIND_PIPE_READ: case FD_KIND_PIPE_WRITE: out->type = 3; break;
        case FD_KIND_SOCK_TCP: case FD_KIND_SOCK_UDP: case FD_KIND_UNIX: out->type = 4; out->mode = 0777; break;
        default: out->type = 6; break;
    }
    out->inode = (uint32_t)(f - open_files) + 1U;
    return 0;
}

int fd_file_path(int descriptor, char* out, int cap, int* cwd)
{
    OpenFile* f = file_of(descriptor);
    if (!f) return -9;
    if (f->kind != FD_KIND_FILE) return -22;
    int l = 0; while (f->path[l] && l + 1 < cap) { out[l] = f->path[l]; ++l; }
    out[l] = 0; *cwd = f->cwd;
    return 0;
}

int fd_ftruncate(int descriptor, uint64_t length)
{
    OpenFile* f = file_of(descriptor);
    if (!f) return -9;
    if (f->kind != FD_KIND_FILE) return -22;
    if (!can_write(f->flags)) return -9;
    if (length > 4194304ULL) return -27;
    return fs_truncate(f->path, f->cwd, length);
}

/* flock: op 1 shared, 2 exclusive, 8 unlock. Returns -11 on conflict (caller blocks unless LOCK_NB). */
int fd_flock(int descriptor, int op)
{
    OpenFile* f = file_of(descriptor);
    if (!f) return -9;
    if (f->kind != FD_KIND_FILE) return -22;
    int obj = (int)(f - open_files);
    int node = fs_resolve(f->path, f->cwd);
    if (node < 0) return -2;
    for (int i = 0; i < FL_MAX; ++i) if (flocks[i].used && flocks[i].obj == obj) flocks[i].used = 0;   /* drop my old lock (convert) */
    if (op == 8) return 0;
    int want_ex = (op == 2);
    for (int i = 0; i < FL_MAX; ++i)
        if (flocks[i].used && flocks[i].node == node && flocks[i].obj != obj && (flocks[i].ex || want_ex)) return -11;
    for (int i = 0; i < FL_MAX; ++i)
        if (!flocks[i].used) { flocks[i].used = 1; flocks[i].node = node; flocks[i].ex = want_ex; flocks[i].obj = obj; return 0; }
    return -37;                                    /* ENOLCK */
}

/* ===================== unified descriptors: sockets, eventfd, AF_UNIX, poll ===================== */

static int new_object_fd(int kind, uint32_t flags, int aux, int owner, int cloexec)
{
    FdTable* table = current_table();
    if (!table) return -24;
    int descriptor = free_descriptor(table, 0);
    if (descriptor < 0) return -24;
    int object = free_object();
    if (object < 0) return -24;
    OpenFile* f = &open_files[object];
    clear_object(object);
    f->used = 1; f->permanent = 0; f->kind = kind; f->refs = 1; f->flags = flags;
    f->aux = aux; f->owner = owner;
    table->entries[descriptor].used = 1;
    table->entries[descriptor].object = object;
    table->entries[descriptor].cloexec = cloexec;
    return descriptor;
}

static OpenFile* file_of(int descriptor)
{
    FdTable* table = current_table();
    if (!descriptor_valid_in(table, descriptor)) return 0;
    return &open_files[table->entries[descriptor].object];
}

static int object_of(int descriptor)
{
    FdTable* table = current_table();
    if (!descriptor_valid_in(table, descriptor)) return -1;
    return table->entries[descriptor].object;
}

int fd_set_nonblock(int descriptor, int value)
{
    OpenFile* f = file_of(descriptor);
    if (!f) return -9;
    if (value) f->flags |= FD_O_NONBLOCK; else f->flags &= ~FD_O_NONBLOCK;
    return 0;
}

int fd_get_flags(int descriptor)
{
    OpenFile* f = file_of(descriptor);
    if (!f) return -9;
    return (int)(f->flags & (3U | FD_O_APPEND | FD_O_NONBLOCK));
}

int fd_is_blocking(int descriptor)
{
    OpenFile* f = file_of(descriptor);
    if (!f || (f->flags & FD_O_NONBLOCK)) return 0;
    return f->kind == FD_KIND_STDIN || f->kind == FD_KIND_PIPE_READ || f->kind == FD_KIND_PIPE_WRITE || f->kind == FD_KIND_SOCK_TCP ||
           f->kind == FD_KIND_SOCK_UDP || f->kind == FD_KIND_EVENT || f->kind == FD_KIND_UNIX || f->kind == FD_KIND_TIMER ||
           f->kind == FD_KIND_SIGNAL || f->kind == FD_KIND_INOTIFY;
}

int fd_is_socket(int descriptor)
{
    OpenFile* f = file_of(descriptor);
    return f && (f->kind == FD_KIND_SOCK_TCP || f->kind == FD_KIND_SOCK_UDP || f->kind == FD_KIND_UNIX);
}

/* ---- AF_UNIX ---- */
static int ux_of(OpenFile* f)
{
    if (!f || f->kind != FD_KIND_UNIX || f->aux < 0 || f->aux >= UX_MAX || !usocks[f->aux].used) return -1;
    return f->aux;
}

static void ring_put(UnixSock* u, const uint8_t* d, uint32_t n)
{
    for (uint32_t i = 0; i < n; ++i) { u->ring[u->wr] = d[i]; u->wr = (u->wr + 1U) % UX_RING; }
    u->size += n; u->tot_w += n; ++u->gen;
}
static void ring_get(UnixSock* u, uint8_t* d, uint32_t n)
{
    for (uint32_t i = 0; i < n; ++i) { if (d) d[i] = u->ring[u->rd]; u->rd = (u->rd + 1U) % UX_RING; }
    u->size -= n; u->tot_r += n;
}

int fd_unix_socket(int type_and_flags)
{
    int type = type_and_flags & SOCK_TYPE_MASK;
    if (type != 1 && type != 2) return -94;      /* ESOCKTNOSUPPORT */
    int idx = ux_alloc(type);
    if (idx < 0) return -24;
    uint32_t fl = FD_O_RDWR | ((type_and_flags & SOCK_NONBLOCK_FLAG) ? FD_O_NONBLOCK : 0);
    int d = new_object_fd(FD_KIND_UNIX, fl, idx, -1, (type_and_flags & SOCK_CLOEXEC_FLAG) != 0);
    if (d < 0) { usocks[idx].used = 0; return d; }
    usocks[idx].attached = 1;
    return d;
}

int fd_socketpair(int type_and_flags, int out[2])
{
    int a = fd_unix_socket(type_and_flags);
    if (a < 0) return a;
    int b = fd_unix_socket(type_and_flags);
    if (b < 0) { fd_close(a); return b; }
    int ia = ux_of(file_of(a)), ib = ux_of(file_of(b));
    usocks[ia].state = usocks[ib].state = 2;
    usocks[ia].peer = ib; usocks[ib].peer = ia;
    out[0] = a; out[1] = b;
    return 0;
}

static int sock_path_ok(const char* path)
{
    int n = string_length(path);
    return path && n > 0 && n < 108;
}

int fd_unix_bind(int descriptor, const char* path)
{
    OpenFile* f = file_of(descriptor);
    int me = ux_of(f);
    if (me < 0) return -88;                       /* ENOTSOCK */
    if (!sock_path_ok(path)) return -22;
    if (usocks[me].path[0]) return -22;
    for (int i = 0; i < UX_MAX; ++i)
        if (usocks[i].used && usocks[i].path[0] && string_length(usocks[i].path) == string_length(path))
        {
            int same = 1;
            for (int k = 0; path[k]; ++k) if (usocks[i].path[k] != path[k]) { same = 0; break; }
            if (same) return -98;                 /* EADDRINUSE */
        }
    copy_string(usocks[me].path, path, 108);
    return 0;
}

static int ux_find_listener(const char* path, int type)
{
    for (int i = 0; i < UX_MAX; ++i)
    {
        if (!usocks[i].used || usocks[i].state != 1 || usocks[i].type != type) continue;
        int k = 0;
        while (path[k] && usocks[i].path[k] == path[k]) ++k;
        if (!path[k] && !usocks[i].path[k]) return i;
    }
    return -1;
}

int fd_unix_connect(int descriptor, const char* path)
{
    OpenFile* f = file_of(descriptor);
    int me = ux_of(f);
    if (me < 0) return -88;
    if (!sock_path_ok(path)) return -22;
    if (usocks[me].state == 2) return -106;       /* EISCONN */
    if (usocks[me].state == 1) return -22;
    int l = ux_find_listener(path, usocks[me].type);
    if (l < 0) return -111;                       /* ECONNREFUSED */
    if (usocks[l].nback >= UX_BACKLOG) return -111;
    int srv = ux_alloc(usocks[me].type);
    if (srv < 0) return -24;
    usocks[srv].state = 2; usocks[srv].peer = me; usocks[srv].attached = 0;
    usocks[me].state = 2; usocks[me].peer = srv;
    usocks[l].backlog[usocks[l].nback++] = srv; ++usocks[l].gen;
    return 0;
}

static int ux_accept(int descriptor, int flags)
{
    OpenFile* f = file_of(descriptor);
    int me = ux_of(f);
    if (me < 0) return -88;
    UnixSock* u = &usocks[me];
    if (u->state != 1) return -22;
    if (u->nback == 0) return -11;
    int srv = u->backlog[0];
    for (int i = 1; i < u->nback; ++i) u->backlog[i - 1] = u->backlog[i];
    --u->nback;
    int d = new_object_fd(FD_KIND_UNIX, FD_O_RDWR | ((flags & SOCK_NONBLOCK_FLAG) ? FD_O_NONBLOCK : 0), srv, -1,
                          (flags & SOCK_CLOEXEC_FLAG) != 0);
    if (d < 0)
    {
        u->backlog[u->nback++] = srv;             /* leave it queued */
        return d;
    }
    usocks[srv].attached = 1;
    return d;
}

/* data path; `fds` may carry SCM_RIGHTS descriptors (stream and datagram) */
static int64_t ux_send(OpenFile* f, const uint8_t* data, uint64_t len, const int* fds, int nfds)
{
    int me = ux_of(f);
    if (me < 0) return -9;
    UnixSock* u = &usocks[me];
    if (u->state != 2) return -107;
    if (u->wr_shut) return -32;
    if (u->peer < 0 || !usocks[u->peer].used) return -32;
    UnixSock* p = &usocks[u->peer];
    if (nfds > 0)
    {
        if (nfds > UX_FDQ || p->nfdq + nfds > UX_FDQ) return -11;
        if (len == 0) return -22;                 /* ancillary data needs at least one byte */
    }
    uint64_t n;
    if (u->type == 2)
    {
        if (len > UX_DGRAM_MAX) return -90;
        if (UX_RING - p->size < len + 2U) return -11;
        uint8_t hdr[2] = { (uint8_t)(len & 0xFF), (uint8_t)(len >> 8) };
        ring_put(p, hdr, 2);
        ring_put(p, data, (uint32_t)len);
        n = len;
    }
    else
    {
        uint64_t space = UX_RING - p->size;
        if (space == 0) return -11;
        n = len < space ? len : space;
        if (nfds > 0 && n < len) return -11;     /* keep the fd-carrying message whole */
        ring_put(p, data, (uint32_t)n);
    }
    for (int i = 0; i < nfds; ++i)
    {
        int obj = object_of(fds[i]);
        if (obj < 0) continue;
        if (!open_files[obj].permanent) ++open_files[obj].refs;
        p->fdq_obj[p->nfdq] = obj; p->fdq_end[p->nfdq] = p->tot_w; ++p->nfdq;
    }
    return (int64_t)n;
}

/* returns bytes, 0 on EOF, -11 when empty. `fds_out`/`nfds_out` receive SCM_RIGHTS if requested */
static int64_t ux_recv(OpenFile* f, uint8_t* out, uint64_t cap, int* fds_out, int* nfds_out, int maxfds)
{
    int me = ux_of(f);
    if (me < 0) return -9;
    UnixSock* u = &usocks[me];
    if (u->state == 1) return -22;
    if (u->state != 2) return -107;
    if (nfds_out) *nfds_out = 0;
    if (u->size == 0)
    {
        if (u->peer_closed || u->peer < 0 || usocks[u->peer].wr_shut) return 0;
        return -11;
    }
    uint64_t n;
    if (u->type == 2)
    {
        uint8_t hdr[2];
        uint32_t save_rd = u->rd;
        (void)save_rd;
        ring_get(u, hdr, 2);
        uint32_t mlen = (uint32_t)hdr[0] | ((uint32_t)hdr[1] << 8);
        n = mlen < cap ? mlen : cap;
        ring_get(u, out, (uint32_t)n);
        if (mlen > n) ring_get(u, 0, mlen - (uint32_t)n);
    }
    else
    {
        n = cap < u->size ? cap : u->size;
        /* never read across the boundary of a message that carries descriptors */
        if (u->nfdq > 0 && u->fdq_end[0] > u->tot_r && u->fdq_end[0] - u->tot_r < n) n = u->fdq_end[0] - u->tot_r;
        ring_get(u, out, (uint32_t)n);
    }
    /* descriptors whose data has been consumed */
    while (u->nfdq > 0 && u->fdq_end[0] <= u->tot_r)
    {
        int obj = u->fdq_obj[0];
        for (int i = 1; i < u->nfdq; ++i) { u->fdq_obj[i - 1] = u->fdq_obj[i]; u->fdq_end[i - 1] = u->fdq_end[i]; }
        --u->nfdq;
        int got = -1;
        if (fds_out && nfds_out && *nfds_out < maxfds)
        {
            FdTable* table = current_table();
            got = attach_object(table, obj, 0);      /* adds a ref */
        }
        if (got >= 0) fds_out[(*nfds_out)++] = got;
        release_object(obj);                          /* drop the reference the queue held */
    }
    return (int64_t)n;
}

int64_t fd_sendmsg(int descriptor, const void* buf, uint64_t len, const int* fds, int nfds)
{
    OpenFile* f = file_of(descriptor);
    if (!f) return -9;
    if (f->kind != FD_KIND_UNIX) return nfds ? -22 : fd_write(descriptor, buf, len);
    if (len > FD_IO_MAX) return -22;
    return ux_send(f, (const uint8_t*)buf, len, fds, nfds);
}

int64_t fd_recvmsg(int descriptor, void* buf, uint64_t cap, int* fds, int* nfds, int maxfds)
{
    OpenFile* f = file_of(descriptor);
    if (!f) return -9;
    if (nfds) *nfds = 0;
    if (f->kind != FD_KIND_UNIX) return fd_read(descriptor, buf, cap);
    if (cap > FD_IO_MAX) cap = FD_IO_MAX;
    return ux_recv(f, (uint8_t*)buf, cap, fds, nfds, maxfds);
}

/* ---- eventfd ---- */
int fd_eventfd(uint64_t initial, int flags)
{
    int idx = -1;
    for (int i = 0; i < EV_MAX; ++i) if (!events[i].used) { idx = i; break; }
    if (idx < 0) return -24;
    events[idx].used = 1; events[idx].counter = initial; events[idx].semaphore = flags & 1;
    int d = new_object_fd(FD_KIND_EVENT, FD_O_RDWR | ((flags & SOCK_NONBLOCK_FLAG) ? FD_O_NONBLOCK : 0), idx, -1,
                          (flags & SOCK_CLOEXEC_FLAG) != 0);
    if (d < 0) events[idx].used = 0;
    return d;
}

/* ---- inet sockets (TCP/UDP) as descriptors ---- */
int fd_socket(int type_and_flags)
{
    int type = type_and_flags & SOCK_TYPE_MASK;
    if (type != 1 && type != 2) return -94;
    int nb = (type_and_flags & SOCK_NONBLOCK_FLAG) ? 1 : 0;
    int cloexec = (type_and_flags & SOCK_CLOEXEC_FLAG) != 0;
    int d = new_object_fd(type == 1 ? FD_KIND_SOCK_TCP : FD_KIND_SOCK_UDP, FD_O_RDWR | (nb ? FD_O_NONBLOCK : 0), -1, -1, cloexec);
    if (d < 0) return d;
    OpenFile* f = file_of(d);
    f->owner = SOCK_OWNER_BASE + object_of(d);
    int h = type == 1 ? net_tcp_socket(f->owner) : net_udp_socket(f->owner);
    if (h < 0) { fd_close(d); return h; }
    f->aux = h;
    return d;
}

static OpenFile* inet_of(int descriptor, int* err)
{
    OpenFile* f = file_of(descriptor);
    if (!f) { *err = -9; return 0; }
    if (f->kind != FD_KIND_SOCK_TCP && f->kind != FD_KIND_SOCK_UDP) { *err = -88; return 0; }
    return f;
}

int fd_sock_bind(int descriptor, uint32_t ip, uint16_t port)
{
    (void)ip;
    int e = 0; OpenFile* f = inet_of(descriptor, &e);
    if (!f) return e;
    if (f->kind == FD_KIND_SOCK_UDP) return net_udp_bind(f->owner, f->aux, port);
    f->aux2 = port;
    return 0;
}

int fd_sock_listen(int descriptor, int backlog)
{
    (void)backlog;
    int e = 0; OpenFile* f = inet_of(descriptor, &e);
    if (!f) return e;
    if (f->kind == FD_KIND_UNIX) return 0;
    if (f->kind != FD_KIND_SOCK_TCP) return -95;     /* EOPNOTSUPP */
    return net_tcp_listen(f->owner, f->aux, (uint16_t)f->aux2);
}

int fd_listen_any(int descriptor, int backlog)
{
    OpenFile* f = file_of(descriptor);
    if (!f) return -9;
    if (f->kind == FD_KIND_UNIX)
    {
        int me = ux_of(f);
        if (me < 0) return -9;
        if (usocks[me].type != 1 || !usocks[me].path[0]) return -22;
        if (usocks[me].state == 2) return -22;
        usocks[me].state = 1;
        return 0;
    }
    return fd_sock_listen(descriptor, backlog);
}

int fd_sock_connect(int descriptor, uint32_t ip, uint16_t port)
{
    int e = 0; OpenFile* f = inet_of(descriptor, &e);
    if (!f) return e;
    if (f->kind == FD_KIND_SOCK_UDP) { f->peer_ip = ip; f->peer_port = port; f->aux2 = 1; return 0; }
    int rc = net_tcp_connect(f->owner, f->aux, ip, port, NET_TCP_MAX_TIMEOUT_MS);
    if (rc == -110) rc = -110;
    return rc;
}

int fd_accept_any(int descriptor, uint32_t* ip, uint16_t* port, int flags)
{
    OpenFile* f = file_of(descriptor);
    if (!f) return -9;
    if (f->kind == FD_KIND_UNIX) return ux_accept(descriptor, flags);
    if (f->kind != FD_KIND_SOCK_TCP) return -95;
    uint32_t pip = 0; uint16_t pport = 0;
    (void)net_poll_once();
    int h = net_tcp_accept(f->owner, f->aux, &pip, &pport, 0);
    if (h < 0) return h;
    int owner = f->owner;
    int d = new_object_fd(FD_KIND_SOCK_TCP, FD_O_RDWR | ((flags & SOCK_NONBLOCK_FLAG) ? FD_O_NONBLOCK : 0), h, owner,
                          (flags & SOCK_CLOEXEC_FLAG) != 0);
    if (d < 0) { (void)net_tcp_close(owner, h); return d; }
    if (ip) *ip = pip;
    if (port) *port = pport;
    return d;
}

int64_t fd_sock_sendto(int descriptor, const void* data, uint64_t len, uint32_t ip, uint16_t port, int has_addr)
{
    int e = 0; OpenFile* f = inet_of(descriptor, &e);
    if (!f) return e;
    if (f->kind == FD_KIND_SOCK_TCP) return net_tcp_send(f->owner, f->aux, data, (uint32_t)len, NET_TCP_MAX_TIMEOUT_MS);
    if (!has_addr)
    {
        if (!f->aux2) return -89;                 /* EDESTADDRREQ */
        ip = f->peer_ip; port = f->peer_port;
    }
    if (len > NET_UDP_MAX_PAYLOAD) return -90;
    return net_udp_sendto(f->owner, f->aux, ip, port, data, (uint32_t)len);
}

int64_t fd_sock_recvfrom(int descriptor, void* data, uint64_t cap, uint32_t* ip, uint16_t* port)
{
    OpenFile* f = file_of(descriptor);
    if (!f) return -9;
    if (f->kind == FD_KIND_UNIX) return fd_recvmsg(descriptor, data, cap, 0, 0, 0);
    if (f->kind == FD_KIND_SOCK_TCP)
    {
        if (cap > NET_TCP_MAX_IO) cap = NET_TCP_MAX_IO;
        (void)net_poll_once();       /* timeout 0 never polls the NIC by itself */
        int r = net_tcp_recv(f->owner, f->aux, data, (uint32_t)cap, 0);
        return r == -110 ? -11 : r;
    }
    if (f->kind != FD_KIND_SOCK_UDP) return -88;
    if (cap > NET_UDP_MAX_PAYLOAD) cap = NET_UDP_MAX_PAYLOAD;
    int r = net_udp_recvfrom(f->owner, f->aux, ip, port, data, (uint32_t)cap, 0);
    return r == -110 ? -11 : r;
}

int fd_sock_shutdown(int descriptor, int how)   /* 0 RD, 1 WR, 2 RDWR */
{
    OpenFile* f = file_of(descriptor);
    if (!f) return -9;
    if (how < 0 || how > 2) return -22;
    if (f->kind == FD_KIND_UNIX)
    {
        int me = ux_of(f);
        if (me < 0) return -9;
        if (usocks[me].state != 2) return -107;
        if (how >= 1) { usocks[me].wr_shut = 1; if (usocks[me].peer >= 0) ++usocks[usocks[me].peer].gen; }
        return 0;
    }
    if (f->kind == FD_KIND_SOCK_TCP)
    {
        if (how >= 1) return net_tcp_shutdown_wr(f->owner, f->aux);
        return 0;
    }
    return f->kind == FD_KIND_SOCK_UDP ? 0 : -88;
}

int fd_sock_name(int descriptor, int peer, uint32_t* ip, uint16_t* port)
{
    OpenFile* f = file_of(descriptor);
    if (!f) return -9;
    if (f->kind == FD_KIND_SOCK_TCP)
    {
        uint32_t rip = 0; uint16_t rport = 0, lport = 0;
        int connected = net_tcp_info(f->owner, f->aux, &rip, &rport, &lport);
        if (connected < 0) return connected;
        if (peer) { if (!connected) return -107; *ip = rip; *port = rport; }
        else { *ip = 0; *port = lport; }
        return 0;
    }
    if (f->kind == FD_KIND_SOCK_UDP)
    {
        if (peer) { if (!f->aux2) return -107; *ip = f->peer_ip; *port = f->peer_port; return 0; }
        *ip = 0; int lp = net_udp_local_port(f->owner, f->aux); *port = lp > 0 ? (uint16_t)lp : 0; return lp < 0 ? lp : 0;
    }
    return -88;
}

/* sockopt SO_ERROR: pending error of a TCP socket (reset -> ECONNRESET) */
int fd_sock_error(int descriptor)
{
    OpenFile* f = file_of(descriptor);
    if (!f) return -9;
    if (f->kind == FD_KIND_SOCK_TCP) return (net_tcp_poll(f->owner, f->aux) & POLL_ERR) ? 104 : 0;
    return 0;
}

int fd_sock_type(int descriptor)   /* SOCK_STREAM 1 / SOCK_DGRAM 2, or -88 */
{
    OpenFile* f = file_of(descriptor);
    if (!f) return -9;
    if (f->kind == FD_KIND_SOCK_TCP) return 1;
    if (f->kind == FD_KIND_SOCK_UDP) return 2;
    if (f->kind == FD_KIND_UNIX) { int me = ux_of(f); return me < 0 ? -9 : usocks[me].type; }
    return -88;
}

int fd_sock_is_unix(int descriptor)
{
    OpenFile* f = file_of(descriptor);
    return f && f->kind == FD_KIND_UNIX;
}

/* ---- readiness (poll) ---- */
int fd_poll_mask(int descriptor)
{
    OpenFile* f = file_of(descriptor);
    if (!f) return POLL_NVAL;
    switch (f->kind)
    {
        case FD_KIND_STDIN: return tty_poll() ? POLL_IN : 0;
        case FD_KIND_STDOUT: case FD_KIND_STDERR: return POLL_OUT;
        case FD_KIND_FILE: return (can_read(f->flags) ? POLL_IN : 0) | (can_write(f->flags) ? POLL_OUT : 0);
        case FD_KIND_PIPE_READ:
        {
            if (f->pipe_id < 0 || !pipes[f->pipe_id].used) return POLL_ERR;
            PipeState* p = &pipes[f->pipe_id];
            return (p->size ? POLL_IN : 0) | (p->writers == 0 ? POLL_HUP : 0);
        }
        case FD_KIND_PIPE_WRITE:
        {
            if (f->pipe_id < 0 || !pipes[f->pipe_id].used) return POLL_ERR;
            PipeState* p = &pipes[f->pipe_id];
            if (p->readers == 0) return POLL_ERR;
            return p->size < FD_PIPE_CAPACITY ? POLL_OUT : 0;
        }
        case FD_KIND_SOCK_TCP: return net_tcp_poll(f->owner, f->aux);
        case FD_KIND_SOCK_UDP: return net_udp_poll(f->owner, f->aux);
        case FD_KIND_EVENT:
        {
            EventState* e = &events[f->aux];
            return (e->counter ? POLL_IN : 0) | (e->counter < 0xFFFFFFFFFFFFFFFEULL ? POLL_OUT : 0);
        }
        case FD_KIND_TIMER: { TimerFd* tf = &timerfds[f->aux]; tf_update(tf); return tf->expirations ? POLL_IN : 0; }
        case FD_KIND_SIGNAL:
        {
            if (f->aux < 0 || f->aux >= SF_MAX || !sigfds[f->aux].used) return POLL_ERR;
            FdTable* t = current_table();
            return t && (process_sig_pending_raw(t->owner_pid) & sigfds[f->aux].mask) ? POLL_IN : 0;
        }
        case FD_KIND_INOTIFY: return f->aux >= 0 && f->aux < IN_INST_MAX && inos[f->aux].used && inos[f->aux].qn ? POLL_IN : 0;
        case FD_KIND_UNIX:
        {
            int me = ux_of(f);
            if (me < 0) return POLL_ERR;
            UnixSock* u = &usocks[me];
            if (u->state == 1) return u->nback ? POLL_IN : 0;
            if (u->state != 2) return POLL_OUT | POLL_HUP;
            int m = 0;
            int eof = u->peer_closed || u->peer < 0 || usocks[u->peer].wr_shut;
            if (u->size) m |= POLL_IN;
            else if (eof) m |= POLL_IN | POLL_HUP;
            if (u->peer >= 0 && usocks[u->peer].used)
            {
                if (UX_RING - usocks[u->peer].size >= (u->type == 2 ? UX_DGRAM_MAX + 2U : 1U) && !u->wr_shut) m |= POLL_OUT;
            }
            else m |= POLL_ERR;
            return m;
        }
        default: return POLL_NVAL;
    }
}


/* Arrival counter for edge-triggered epoll: changes whenever new data, EOF, an error or a new connection shows up on the
   descriptor.  Returns (1<<32)|counter, or 0 for kinds that have no counter (files, tty): the caller then falls back to
   comparing readiness masks. */
int64_t fd_poll_gen(int descriptor)
{
    OpenFile* f = file_of(descriptor);
    if (!f) return -9;
    switch (f->kind)
    {
        case FD_KIND_PIPE_READ: if (f->pipe_id < 0 || !pipes[f->pipe_id].used) return 0; return (1LL << 32) | pipes[f->pipe_id].gen;
        case FD_KIND_EVENT: return (1LL << 32) | events[f->aux].gen;
        case FD_KIND_TIMER: { TimerFd* tf = &timerfds[f->aux]; tf_update(tf); return (1LL << 32) | tf->gen; }
        case FD_KIND_UNIX: { int me = ux_of(f); if (me < 0) return 0; return (1LL << 32) | usocks[me].gen; }
        case FD_KIND_SIGNAL: { FdTable* t = current_table(); return t ? ((1LL << 32) | process_sig_serial(t->owner_pid)) : 0; }
        case FD_KIND_INOTIFY: return f->aux >= 0 && f->aux < IN_INST_MAX ? ((1LL << 32) | inos[f->aux].gen) : 0;
        case FD_KIND_SOCK_TCP: { (void)net_poll_once(); uint32_t g = net_tcp_gen(f->owner, f->aux); return (1LL << 32) | g; }
        case FD_KIND_SOCK_UDP: { (void)net_poll_once(); uint32_t g = net_udp_gen(f->owner, f->aux); return (1LL << 32) | g; }
        default: return 0;
    }
}

int64_t fd_read(int descriptor, void* buffer, uint64_t length)
{
    FdTable* table = current_table();
    if (!descriptor_valid_in(table, descriptor)) return -9;
    if (length > FD_IO_MAX) return -22;
    if (length == 0) return 0;
    if (!buffer) return -14;

    OpenFile* entry = &open_files[table->entries[descriptor].object];
    if (!can_read(entry->flags)) return -9;

    if (entry->kind == FD_KIND_STDIN)
    {
        return tty_read((uint8_t*)buffer, length);
    }
    if (entry->kind == FD_KIND_PIPE_READ)
    {
        int pipe_id = entry->pipe_id;
        if (pipe_id < 0 || pipe_id >= FD_PIPE_MAX || !pipes[pipe_id].used) return -5;
        PipeState* pipe = &pipes[pipe_id];
        if (pipe->size == 0) return pipe->writers == 0 ? 0 : -11;
        uint64_t count = length < pipe->size ? length : pipe->size;
        uint8_t* out = (uint8_t*)buffer;
        for (uint64_t i = 0; i < count; ++i)
        {
            out[i] = pipe->data[pipe->read_pos];
            pipe->read_pos = (pipe->read_pos + 1U) % FD_PIPE_CAPACITY;
        }
        pipe->size -= (uint32_t)count;
        return (int64_t)count;
    }
    if (entry->kind == FD_KIND_SOCK_TCP || entry->kind == FD_KIND_SOCK_UDP)
        return fd_sock_recvfrom(descriptor, buffer, length, 0, 0);
    if (entry->kind == FD_KIND_UNIX) return fd_recvmsg(descriptor, buffer, length, 0, 0, 0);
    if (entry->kind == FD_KIND_TIMER)
    {
        if (length < 8) return -22;
        TimerFd* tf = &timerfds[entry->aux];
        tf_update(tf);
        if (tf->expirations == 0) return -11;
        *(uint64_t*)buffer = tf->expirations;
        tf->expirations = 0;
        return 8;
    }
    if (entry->kind == FD_KIND_SIGNAL) return signalfd_read(entry, buffer, length);
    if (entry->kind == FD_KIND_INOTIFY) return inotify_read(entry, buffer, length);
    if (entry->kind == FD_KIND_EVENT)
    {
        if (length < 8) return -22;
        EventState* ev = &events[entry->aux];
        if (ev->counter == 0) return -11;
        uint64_t v = ev->semaphore ? 1 : ev->counter;
        ev->counter -= v;
        *(uint64_t*)buffer = v;
        return 8;
    }
    if (entry->kind != FD_KIND_FILE) return -9;

    uint64_t size = 0;
    int rc = fs_read_file(entry->path, entry->cwd, file_buffer,
                          sizeof(file_buffer), &size);
    if (rc != 0) return rc == -3 ? -27 : rc;
    if (entry->offset >= size) return 0;

    uint64_t available = size - entry->offset;
    uint64_t count = length < available ? length : available;
    uint8_t* out = (uint8_t*)buffer;
    for (uint64_t i = 0; i < count; ++i)
        out[i] = file_buffer[entry->offset + i];
    entry->offset += count;
    if (ino_active && count) ino_file_event(entry->path, entry->cwd, 0x1U);      /* IN_ACCESS */
    return (int64_t)count;
}

int64_t fd_write(int descriptor, const void* buffer, uint64_t length)
{
    FdTable* table = current_table();
    if (!descriptor_valid_in(table, descriptor)) return -9;
    if (length > FD_IO_MAX) return -22;
    if (length == 0) return 0;
    if (!buffer) return -14;

    OpenFile* entry = &open_files[table->entries[descriptor].object];
    if (!can_write(entry->flags)) return -9;

    if (entry->kind == FD_KIND_STDOUT || entry->kind == FD_KIND_STDERR)
    {
        const char* bytes = (const char*)buffer;
        for (uint64_t i = 0; i < length; ++i) uart_putc(bytes[i]);
        return (int64_t)length;
    }
    if (entry->kind == FD_KIND_PIPE_WRITE)
    {
        int pipe_id = entry->pipe_id;
        if (pipe_id < 0 || pipe_id >= FD_PIPE_MAX || !pipes[pipe_id].used) return -5;
        PipeState* pipe = &pipes[pipe_id];
        if (pipe->readers == 0) return -32;
        uint64_t available = FD_PIPE_CAPACITY - pipe->size;
        if (available == 0) return -11;
        uint64_t count = length < available ? length : available;
        const uint8_t* input = (const uint8_t*)buffer;
        for (uint64_t i = 0; i < count; ++i)
        {
            pipe->data[pipe->write_pos] = input[i];
            pipe->write_pos = (pipe->write_pos + 1U) % FD_PIPE_CAPACITY;
        }
        pipe->size += (uint32_t)count; ++pipe->gen;
        return (int64_t)count;
    }
    if (entry->kind == FD_KIND_SOCK_TCP || entry->kind == FD_KIND_SOCK_UDP)
        return fd_sock_sendto(descriptor, buffer, length, 0, 0, 0);
    if (entry->kind == FD_KIND_UNIX) return fd_sendmsg(descriptor, buffer, length, 0, 0);
    if (entry->kind == FD_KIND_EVENT)
    {
        if (length < 8) return -22;
        uint64_t v = *(const uint64_t*)buffer;
        EventState* ev = &events[entry->aux];
        if (v == 0xFFFFFFFFFFFFFFFFULL) return -22;
        if (ev->counter > 0xFFFFFFFFFFFFFFFEULL - v) return -11;
        ev->counter += v; ++ev->gen;
        return 8;
    }
    if (entry->kind != FD_KIND_FILE) return -9;

    uint64_t old_size = 0;
    int exists = fs_read_file_for_update(entry->path, entry->cwd, file_buffer,
                                         sizeof(file_buffer), &old_size);
    if (exists != 0) return exists == -2 ? -2 : (exists == -3 ? -27 : -5);

    if (entry->flags & FD_O_APPEND) entry->offset = old_size;
    if (entry->offset > FD_FILE_MAX || length > FD_FILE_MAX - entry->offset)
        return -27;
    {   /* RLIMIT_FSIZE: nothing may be written at or past the limit (SIGXFSZ + EFBIG); a write crossing it is shortened */
        uint64_t fl = process_rlimit_cur(table->owner_pid, PROC_RLIMIT_FSIZE);
        if (fl != PROC_RLIM_INF && entry->offset + length > fl)
        {
            if (entry->offset >= fl) { (void)process_sig_raise(table->owner_pid, 25); return -27; }
            length = fl - entry->offset;
        }
    }

    uint64_t end = entry->offset + length;
    if (entry->offset > old_size)
        for (uint64_t i = old_size; i < entry->offset; ++i)
            file_buffer[i] = 0;

    const uint8_t* input = (const uint8_t*)buffer;
    for (uint64_t i = 0; i < length; ++i)
        file_buffer[entry->offset + i] = input[i];

    uint64_t new_size = end > old_size ? end : old_size;
    int write_rc = fs_write_data(entry->path, entry->cwd, file_buffer, new_size);
    if (write_rc != 0) return write_rc;

    entry->offset = end;
    return (int64_t)length;
}

int64_t fd_seek(int descriptor, int64_t offset, int whence)
{
    FdTable* table = current_table();
    if (!descriptor_valid_in(table, descriptor)) return -9;
    OpenFile* entry = &open_files[table->entries[descriptor].object];
    if (entry->kind != FD_KIND_FILE) return -29;

    int id = fs_resolve(entry->path, entry->cwd);
    if (id < 0 || fs_get_type(id) != FS_FILE) return -2;
    uint64_t size = fs_file_size(id);

    int64_t base = 0;
    if (whence == FD_SEEK_SET) base = 0;
    else if (whence == FD_SEEK_CUR) base = (int64_t)entry->offset;
    else if (whence == FD_SEEK_END) base = (int64_t)size;
    else return -22;

    if ((offset > 0 && base > 0x7FFFFFFFFFFFFFFFLL - offset) ||
        (offset < 0 && base < (-0x7FFFFFFFFFFFFFFFLL - 1LL) - offset))
        return -22;
    int64_t target = base + offset;
    if (target < 0 || (uint64_t)target > FD_FILE_MAX) return -22;

    entry->offset = (uint64_t)target;
    return target;
}

void fd_list()
{
    FdTable* table = current_table();
    uart_puts("FD   TYPE       MODE       OFFSET  PATH\r\n");
    if (!table) { uart_puts("(no descriptor table)\r\n"); return; }
    for (int i = 0; i < FD_MAX_DESCRIPTORS; ++i)
    {
        if (!descriptor_valid_in(table, i)) continue;
        OpenFile* entry = &open_files[table->entries[i].object];
        print_u64((uint64_t)i); uart_puts("    ");
        switch (entry->kind)
        {
            case FD_KIND_STDIN: uart_puts("stdin      "); break;
            case FD_KIND_STDOUT: uart_puts("stdout     "); break;
            case FD_KIND_STDERR: uart_puts("stderr     "); break;
            case FD_KIND_FILE: uart_puts("file       "); break;
            case FD_KIND_PIPE_READ: uart_puts("pipe-read  "); break;
            case FD_KIND_PIPE_WRITE: uart_puts("pipe-write "); break;
            case FD_KIND_SOCK_TCP: uart_puts("tcp-socket "); break;
            case FD_KIND_SOCK_UDP: uart_puts("udp-socket "); break;
            case FD_KIND_EVENT: uart_puts("eventfd    "); break;
            case FD_KIND_UNIX: uart_puts("unix-socket"); break;
            case FD_KIND_TIMER: uart_puts("timerfd    "); break;
            case FD_KIND_SIGNAL: uart_puts("signalfd   "); break;
            case FD_KIND_INOTIFY: uart_puts("inotify    "); break;
            default: uart_puts("unknown    "); break;
        }
        if (entry->kind == FD_KIND_FILE)
        {
            uint32_t mode = access_mode(entry->flags);
            uart_puts(mode == FD_O_RDONLY ? "read      " :
                      mode == FD_O_WRONLY ? "write     " : "read/write");
        }
        else uart_puts("-         ");
        uart_puts(" "); print_u64(entry->offset); uart_puts("  ");
        uart_puts(entry->path[0] ? entry->path : "-"); uart_puts("\r\n");
    }
}


/* ---- process creation support ------------------------------------------ */

/* fork(): the child's table references the very same open-file objects as the
   parent's (shared offsets, shared pipe ends), like POSIX descriptors. */
int fd_clone_table(int src_pid, int dst_pid)
{
    int src = find_table(src_pid);
    if (src < 0) return -1;
    int dst = find_table(dst_pid);
    if (dst < 0) dst = create_table(dst_pid);
    if (dst < 0) return -24;
    FdTable* a = &tables[src];
    FdTable* b = &tables[dst];
    for (int d = 0; d < FD_MAX_DESCRIPTORS; ++d) close_entry(b, d);
    for (int d = 0; d < FD_MAX_DESCRIPTORS; ++d)
    {
        clear_descriptor(b, d);
        if (!descriptor_valid_in(a, d)) continue;
        int object = a->entries[d].object;
        b->entries[d].used = 1;
        b->entries[d].object = object;
        b->entries[d].cloexec = a->entries[d].cloexec;
        if (!open_files[object].permanent) ++open_files[object].refs;
    }
    return 0;
}

/* Make descriptor `dst_fd` of `dst_pid` refer to descriptor `src_fd` of `src_pid`
   (used by the shell to wire `a | b` between two freshly spawned programs). */
int fd_redirect(int dst_pid, int dst_fd, int src_pid, int src_fd)
{
    int s = find_table(src_pid);
    int d = find_table(dst_pid);
    if (s < 0 || d < 0 || dst_fd < 0 || dst_fd >= FD_MAX_DESCRIPTORS) return -9;
    if (!descriptor_valid_in(&tables[s], src_fd)) return -9;
    close_entry(&tables[d], dst_fd);
    int object = tables[s].entries[src_fd].object;
    tables[d].entries[dst_fd].used = 1;
    tables[d].entries[dst_fd].object = object;
    if (!open_files[object].permanent) ++open_files[object].refs;
    return 0;
}

int fd_close_in(int pid, int descriptor)
{
    int t = find_table(pid);
    if (t < 0 || descriptor < 0 || descriptor >= FD_MAX_DESCRIPTORS) return -9;
    close_entry(&tables[t], descriptor);
    return 0;
}

int fd_is_pipe(int descriptor)
{
    FdTable* table = current_table();
    if (!descriptor_valid_in(table, descriptor)) return 0;
    int k = open_files[table->entries[descriptor].object].kind;
    return k == FD_KIND_PIPE_READ || k == FD_KIND_PIPE_WRITE;
}

/* FD_CLOEXEC handling (fcntl F_GETFD/F_SETFD and exec) */
int fd_get_cloexec(int descriptor)
{
    FdTable* table = current_table();
    if (!descriptor_valid_in(table, descriptor)) return -9;
    return table->entries[descriptor].cloexec ? 1 : 0;
}
int fd_set_cloexec(int descriptor, int value)
{
    FdTable* table = current_table();
    if (!descriptor_valid_in(table, descriptor)) return -9;
    table->entries[descriptor].cloexec = value ? 1 : 0;
    return 0;
}
void fd_close_on_exec(int pid)
{
    int t = find_table(pid);
    if (t < 0) return;
    for (int d = 0; d < FD_MAX_DESCRIPTORS; ++d)
        if (tables[t].entries[d].used && tables[t].entries[d].cloexec) close_entry(&tables[t], d);
}

/* Path, directory and open flags of a regular-file descriptor (for MAP_SHARED write-back). */
int fd_file_info(int descriptor, char* path, int cap, int* cwd, uint32_t* flags)
{
    FdTable* table = current_table();
    if (!descriptor_valid_in(table, descriptor)) return -9;
    OpenFile* entry = &open_files[table->entries[descriptor].object];
    if (entry->kind != FD_KIND_FILE) return -19;   /* ENODEV */
    copy_string(path, entry->path, cap);
    if (cwd) *cwd = entry->cwd;
    if (flags) *flags = entry->flags;
    return 0;
}
