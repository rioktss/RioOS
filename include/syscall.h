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

#ifndef SYSCALL_H
#define SYSCALL_H

#include "types.h"
#include "exception_frame.h"

enum SyscallNumber
{
    SYS_PING       = 0,
    SYS_WRITE      = 1,
    SYS_UPTIME     = 2,
    SYS_GETPID     = 3,
    SYS_SCHED_TICK = 4,
    SYS_MEM_FREE   = 5,
    SYS_EXIT       = 6,
    SYS_UNAME      = 7,
    SYS_WRITE_BUF  = 8,   /* write(buf, len) -> bytes written          */
    SYS_SLEEP_MS   = 9,   /* sleep(ms), ms <= 10000                    */
    SYS_READ       = 10,  /* read(buf, len) non-blocking -> bytes read */
    SYS_TIME_MS    = 11,  /* milliseconds since boot                   */
    SYS_FILE_READ  = 12,  /* file_read(path, buf, cap) -> bytes read   */
    SYS_FILE_WRITE = 13,  /* file_write(path, buf, len) -> bytes written */
    SYS_GETARG     = 14,  /* getarg(buf, cap) -> length of argument string */
    SYS_FD_OPEN    = 15,  /* fd_open(path, flags) -> descriptor */
    SYS_FD_CLOSE   = 16,  /* fd_close(fd) */
    SYS_FD_READ    = 17,  /* fd_read(fd, buffer, length) */
    SYS_FD_WRITE   = 18,  /* fd_write(fd, buffer, length) */
    SYS_FD_SEEK    = 19,  /* fd_seek(fd, offset, whence) */
    SYS_FD_DUP     = 20,  /* fd_dup(oldfd) */
    SYS_FD_DUP2    = 21,  /* fd_dup2(oldfd, newfd) */
    SYS_FD_PIPE    = 22,  /* fd_pipe(int descriptors[2]), non-blocking */
    SYS_FS_STAT    = 23,  /* stat(path, FsStat*) */
    SYS_FS_TRUNCATE= 24,  /* truncate(path, length) */
    SYS_FS_CHMOD   = 25,  /* chmod(path, mode), owner or root */
    SYS_NET_UDP_SOCKET   = 26, /* udp_socket() -> owner-scoped handle */
    SYS_NET_UDP_BIND     = 27, /* udp_bind(handle, local_port) */
    SYS_NET_UDP_SENDTO   = 28, /* udp_sendto(handle, ip, port, data, length) */
    SYS_NET_UDP_RECVFROM = 29, /* udp_recvfrom(handle, &ip, &port, data, cap, timeout_ms) */
    SYS_NET_UDP_CLOSE    = 30, /* udp_close(handle) */
    SYS_NET_DNS_LOOKUP   = 31, /* dns_lookup(name, &ip, timeout_ms) */
    SYS_NET_TCP_SOCKET   = 32, /* tcp_socket() -> owner-scoped handle */
    SYS_NET_TCP_CONNECT  = 33, /* tcp_connect(handle, ip, port, timeout_ms) */
    SYS_NET_TCP_SEND     = 34, /* tcp_send(handle, data, length, timeout_ms) */
    SYS_NET_TCP_RECV     = 35, /* tcp_recv(handle, data, capacity, timeout_ms) */
    SYS_NET_TCP_CLOSE    = 36, /* tcp_close(handle) */
    SYS_FORK       = 37,  /* fork() -> child pid in parent, 0 in child (copy-on-write) */
    SYS_EXEC       = 38,  /* exec(path, args) replaces the process image */
    SYS_WAITPID    = 39,  /* waitpid(pid|-1, int* status, options) */
    SYS_GETPPID    = 40,
    SYS_KILL       = 41,  /* kill(pid, signal) */
    SYS_YIELD      = 42,
    SYS_BRK        = 43,  /* brk(new_end) -> current end (0 queries) */
    SYS_MMAP       = 44,  /* mmap(length, prot, fd, offset) anonymous (fd<0) or private file copy */
    SYS_MUNMAP     = 45,  /* munmap(addr, length) */
    SYS_SPAWN      = 46,  /* spawn(path, args, flags) -> pid (flags bit0 = background) */
    SYS_PROCINFO   = 47,  /* procinfo(pid, ProcInfo*) */
    SYS_PIPE_WAIT  = 48,  /* pipe2(int fds[2]) blocking pipe (same as fd_pipe; reads/writes block) */
    SYS_SPAWN_FD   = 49,  /* spawn_fd(path, args, in_fd, out_fd) wires the child's stdin/stdout */
    SYS_MPROTECT   = 50,  /* mprotect(addr, length, prot) on mmap/brk memory (W^X enforced) */
    SYS_GETUID     = 51,  /* getuid() -> uid | gid << 32 */
    SYS_NET_TCP_LISTEN = 53, /* tcp_listen(handle, port) */
    SYS_FS_MKDIR   = 55,  /* mkdir(path) */
    SYS_FS_UNLINK  = 56,  /* unlink(path): file, symlink or empty directory */
    SYS_FS_RENAME  = 57,  /* rename(from, to) within one mount */
    SYS_FS_READDIR = 58,  /* readdir(path, buf, cap): lines "<f|d|l> <size> <name>\n" -> bytes, or -errno */
    SYS_TIME_EPOCH = 59,
    SYS_FS_SYMLINK = 60,  /* symlink(target, linkpath) */
    SYS_FS_READLINK= 61,  /* readlink(path, buf, cap) -> length (no NUL) or -errno */
    SYS_FS_LINK    = 62,  /* link(existing, newpath): hard link, same filesystem */
    SYS_FS_LSTAT   = 63,  /* lstat(path, FsStat*): does not follow a final symlink */
    SYS_FS_REALPATH= 64,  /* realpath(path, buf, cap) -> length or -errno; follows every symlink */  /* seconds since 1970 (RTC) */
    SYS_SIGACTION  = 65,  /* sigaction(sig, handler|0 default|1 ignore, restorer, amask | flags<<32) -> old handler */
    SYS_SIGPROCMASK= 66,  /* sigprocmask(how 0 block/1 unblock/2 set/3 query, mask) -> old mask */
    SYS_SIGRETURN  = 67,  /* returns from a signal handler (called by the libc restorer) */
    SYS_ALARM_MS   = 68,  /* alarm_ms(ms) -> previous remaining ms; 0 cancels; raises SIGALRM */
    SYS_PAUSE      = 69,  /* pause(): waits for a deliverable signal, -EINTR */
    SYS_SIGPENDING = 70,  /* sigpending() -> mask of pending but blocked signals */
    SYS_FCNTL      = 71,  /* fcntl(fd, cmd 0 DUPFD / 1 GETFD / 2 SETFD, arg) */
    SYS_MMAPX      = 72,  /* mmapx(length, prot, fd, off | flags<<32): MAP_SHARED/PRIVATE/ANON */
    SYS_MSYNC      = 73,  /* msync(addr, length): write shared file pages back */
    SYS_SIGQUEUE   = 74,  /* sigqueue(pid, sig, value) */
    SYS_SIGALTSTACK= 75,  /* sigaltstack(const stack_t* new, stack_t* old) */
    SYS_SIGWAIT    = 76,  /* sigwait(set, timeout_ms | ~0 = forever, siginfo* out) -> signal / -EAGAIN / -EINTR */
    SYS_SOCKET     = 80,  /* socket(domain 1 UNIX / 2 INET, type 1 STREAM / 2 DGRAM | 0x800 NONBLOCK | 0x1000 CLOEXEC) -> fd */
    SYS_BIND       = 81,  /* bind(fd, ip, port) or bind(fd, path) for AF_UNIX */
    SYS_LISTEN     = 82,  /* listen(fd, backlog) */
    SYS_ACCEPT     = 83,  /* accept(fd, uint64_t* peer(ip|port<<32) or 0, flags) -> fd (blocks unless O_NONBLOCK) */
    SYS_CONNECT    = 84,  /* connect(fd, ip, port) or connect(fd, path) for AF_UNIX */
    SYS_SENDTO     = 85,  /* sendto(fd, buf, len, ip|port<<32, has_addr) */
    SYS_RECVFROM   = 86,  /* recvfrom(fd, buf, len, uint64_t* peer or 0) */
    SYS_SHUTDOWN   = 87,  /* shutdown(fd, how) */
    SYS_SOCKNAME   = 88,  /* sockname(fd, peer, uint64_t* out(ip|port<<32)) */
    SYS_SOCKETPAIR = 89,  /* socketpair(type|flags, int fds[2]) AF_UNIX */
    SYS_EVENTFD    = 90,  /* eventfd(initial, flags: 1 SEMAPHORE | 0x800 NONBLOCK | 0x1000 CLOEXEC) */
    SYS_POLL       = 91,  /* poll(struct pollfd*, nfds, timeout_ms (<0 forever)) -> ready count / -EINTR */
    SYS_SENDMSG    = 92,  /* sendmsg(fd, buf, len, int* fds, nfds): SCM_RIGHTS */
    SYS_RECVMSG    = 93,  /* recvmsg(fd, buf, cap, int* fds, maxfds | nfds_out_ptr<<8 not used: x4 = int* nfds) */
    SYS_SOCKINFO   = 94,  /* sockinfo(fd, what 0 SO_ERROR / 1 SO_TYPE) */
    SYS_PIPE2      = 95,  /* pipe2(int fds[2], flags 0x800 NONBLOCK | 0x1000 CLOEXEC) */
    SYS_FUTEX      = 96,  /* futex(addr, op 0 WAIT / 1 WAKE, val, timeout_ms | ~0 forever) */
    SYS_FSTAT      = 98,  /* fstat(fd, FsStat*) */
    SYS_FS_CHOWN   = 99,  /* chown(path, uid, gid): root only */
    SYS_FS_UTIME   = 100, /* utime(path, mtime_seconds): owner or root */
    SYS_FTRUNCATE  = 101, /* ftruncate(fd, length) */
    SYS_FLOCK      = 102, /* flock(fd, op 1 SH / 2 EX / 8 UN | 4 NB) */
    SYS_FSYNC      = 103, /* fsync(fd): flushes the filesystem */
    SYS_GETRANDOM  = 104, /* getrandom(buf, len <= 4096) */
    SYS_TIMERFD    = 105, /* timerfd(op 0 create flags / 1 settime fd,first_ms,interval_ms,old* / 2 gettime fd,out*) */
    SYS_GETPW      = 106, /* getpw(index, char out[24]: name[16], uid u32, gid u32) -> 0 / -ENOENT past the end */
    SYS_CHDIR      = 107, /* chdir(path): per-process working directory */
    SYS_GETCWD     = 108, /* getcwd(buf, cap) -> length */
    SYS_UMASK      = 109, /* umask(mask | ~0 queries) -> old mask */
    SYS_SYSINFO    = 110, /* sysinfo(uint64_t out[4]: total_pages, free_pages, online_cpus, uptime_ms) */
    SYS_ED25519    = 112, /* ed25519(op, uint64_t a[4]): op 0 pubkey(seed,-,-,pub) 1 sign(seed,msg,len,sig) 2 verify(pub,msg,len,sig) 3 rng-ready */
    SYS_FDATTR     = 111, /* fdattr(fd, op 0 fchmod(mode) / 1 fchown(uid,gid) / 2 futimens(mtime_seconds)) on regular files */
    SYS_TTY        = 97,  /* tty(op 0 tcgetattr / 1 tcsetattr(action in x2) / 2 winsize, ptr) */
    SYS_HTTP_GET   = 78,  /* http_get(url, buf, cap, flags | cacert<<8?) see syscall.cpp: flags bit0 = insecure; x4 = cacert path or 0 -> body length or -errno */
    SYS_SHA256     = 79,  /* sha256(data, len, out32) -> 0 */
    SYS_SETITIMER  = 77,  /* setitimer(value_ms, interval_ms) -> previous remaining ms (SIGALRM) */
    SYS_NET_TCP_ACCEPT = 54, /* tcp_accept(handle, uint64_t* peer(ip|port<<32) or 0, timeout_ms) -> new handle */
    SYS_SETUID     = 52,  /* setuid(uid, gid): root only (drops privilege irreversibly for non-root targets) */
};

void syscall_init();

uint64_t syscall_invoke(
    uint64_t number,
    uint64_t a0 = 0,
    uint64_t a1 = 0,
    uint64_t a2 = 0,
    uint64_t a3 = 0,
    uint64_t a4 = 0,
    uint64_t a5 = 0
);

void syscall_dispatch(ExceptionFrame* frame, int from_user);
int syscall_user_pointer_ok(const void* pointer, uint64_t length);
int syscall_user_pointer_ok_write(const void* pointer, uint64_t length);

struct ProcInfo
{
    int32_t pid, ppid, state, thread;
    uint32_t uid, gid;
    uint64_t start_ms, resident_pages, shared_pages, cow_faults, demand_faults;
    char name[32];
};

#define SYSCALL_IO_MAX 4096ULL
#define SYSCALL_SLEEP_MAX_MS 10000ULL
#define SYSCALL_PATH_MAX 128ULL
#define SYSCALL_FILE_MAX 4096ULL

#endif
