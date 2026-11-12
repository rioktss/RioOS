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

#include "ulibc.h"
#include "../../include/user_api.h"
/* RioOS libc part 6: BSD sockets (AF_INET stream/dgram, AF_UNIX stream/dgram + SCM_RIGHTS), poll/select, eventfd,
   socketpair, pipe2, readv/writev, name resolution (getaddrinfo/gethostbyname), inet_* helpers. Sockets are ordinary descriptors. */

#define K_SOCKET 80
#define K_BIND 81
#define K_LISTEN 82
#define K_ACCEPT 83
#define K_CONNECT 84
#define K_SENDTO 85
#define K_RECVFROM 86
#define K_SHUTDOWN 87
#define K_SOCKNAME 88
#define K_SOCKETPAIR 89
#define K_EVENTFD 90
#define K_POLL 91
#define K_SENDMSG 92
#define K_RECVMSG 93
#define K_SOCKINFO 94
#define K_PIPE2 95

extern int __rio_sa_restart;
static long sret(uint64_t v) { long r = (long)v; if (r < 0 && r > -4096) { errno = (int)-r; return -1; } return r; }

/* ---- byte order / address text --------------------------------------------------------------------------------- */
uint16_t htons(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }
uint16_t ntohs(uint16_t v) { return htons(v); }
uint32_t htonl(uint32_t v) { return (v >> 24) | ((v >> 8) & 0xFF00U) | ((v << 8) & 0xFF0000U) | (v << 24); }
uint32_t ntohl(uint32_t v) { return htonl(v); }

static int parse_v4(const char* s, uint32_t* host_order)
{
    uint32_t parts[4]; int n = 0;
    if (!s || !*s) return 0;
    for (;;)
    {
        if (*s < '0' || *s > '9') return 0;
        uint32_t v = 0; int digits = 0;
        while (*s >= '0' && *s <= '9') { v = v * 10U + (uint32_t)(*s - '0'); if (v > 255U || ++digits > 3) return 0; ++s; }
        parts[n++] = v;
        if (*s == '.') { ++s; if (n == 4) return 0; continue; }
        break;
    }
    if (*s || n != 4) return 0;
    *host_order = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
    return 1;
}
int inet_aton(const char* s, struct in_addr* out) { uint32_t h; if (!parse_v4(s, &h)) return 0; if (out) out->s_addr = htonl(h); return 1; }
in_addr_t inet_addr(const char* s) { struct in_addr a; return inet_aton(s, &a) ? a.s_addr : INADDR_NONE; }
static char ntoa_buf[16];
static void fmt_v4(uint32_t host, char* out)
{
    int n = 0;
    for (int i = 3; i >= 0; --i)
    {
        unsigned v = (host >> (i * 8)) & 0xFF;
        if (v >= 100) out[n++] = (char)('0' + v / 100);
        if (v >= 10) out[n++] = (char)('0' + (v / 10) % 10);
        out[n++] = (char)('0' + v % 10);
        if (i) out[n++] = '.';
    }
    out[n] = 0;
}
char* inet_ntoa(struct in_addr a) { fmt_v4(ntohl(a.s_addr), ntoa_buf); return ntoa_buf; }
int inet_pton(int af, const char* src, void* dst)
{
    if (af != AF_INET) { errno = EAFNOSUPPORT; return -1; }
    uint32_t h; if (!parse_v4(src, &h)) return 0;
    ((struct in_addr*)dst)->s_addr = htonl(h); return 1;
}
const char* inet_ntop(int af, const void* src, char* dst, socklen_t size)
{
    if (af != AF_INET) { errno = EAFNOSUPPORT; return 0; }
    if (size < INET_ADDRSTRLEN) { errno = 28; return 0; }
    fmt_v4(ntohl(((const struct in_addr*)src)->s_addr), dst);
    return dst;
}

/* ---- sockets ---------------------------------------------------------------------------------------------------- */
#define MAXFD 24
static unsigned rcv_to[MAXFD], snd_to[MAXFD];     /* SO_RCVTIMEO / SO_SNDTIMEO in ms, 0 = none */
extern void __rio_epoll_close(int fd);
void __rio_sockopt_close(int fd) { if (fd >= 0 && fd < MAXFD) rcv_to[fd] = snd_to[fd] = 0; __rio_epoll_close(fd); }

int socket(int domain, int type, int protocol)
{
    (void)protocol;
    return (int)sret(user_syscall2(K_SOCKET, (uint64_t)domain, (uint64_t)type));
}
int socketpair(int domain, int type, int protocol, int sv[2])
{
    (void)protocol;
    if (domain != AF_UNIX) { errno = EAFNOSUPPORT; return -1; }
    return (int)sret(user_syscall2(K_SOCKETPAIR, (uint64_t)type, (uint64_t)sv));
}

int bind(int fd, const struct sockaddr* addr, socklen_t len)
{
    if (!addr || len < 2) { errno = EINVAL; return -1; }
    if (addr->sa_family == AF_UNIX)
    {
        const struct sockaddr_un* un = (const struct sockaddr_un*)addr;
        return (int)sret(user_syscall2(K_BIND, (uint64_t)fd, (uint64_t)un->sun_path));
    }
    if (addr->sa_family != AF_INET || len < sizeof(struct sockaddr_in)) { errno = EAFNOSUPPORT; return -1; }
    const struct sockaddr_in* in = (const struct sockaddr_in*)addr;
    return (int)sret(user_syscall3(K_BIND, (uint64_t)fd, ntohl(in->sin_addr.s_addr), ntohs(in->sin_port)));
}
int listen(int fd, int backlog) { return (int)sret(user_syscall2(K_LISTEN, (uint64_t)fd, (uint64_t)backlog)); }
int connect(int fd, const struct sockaddr* addr, socklen_t len)
{
    if (!addr || len < 2) { errno = EINVAL; return -1; }
    if (addr->sa_family == AF_UNIX)
    {
        const struct sockaddr_un* un = (const struct sockaddr_un*)addr;
        return (int)sret(user_syscall2(K_CONNECT, (uint64_t)fd, (uint64_t)un->sun_path));
    }
    if (addr->sa_family != AF_INET || len < sizeof(struct sockaddr_in)) { errno = EAFNOSUPPORT; return -1; }
    const struct sockaddr_in* in = (const struct sockaddr_in*)addr;
    return (int)sret(user_syscall3(K_CONNECT, (uint64_t)fd, ntohl(in->sin_addr.s_addr), ntohs(in->sin_port)));
}

static void fill_in(struct sockaddr* addr, socklen_t* len, uint64_t packed, int have)
{
    if (!addr || !len) return;
    struct sockaddr_in in; memset(&in, 0, sizeof in);
    if (have)
    {
        in.sin_family = AF_INET;
        in.sin_addr.s_addr = htonl((uint32_t)packed);
        in.sin_port = htons((uint16_t)(packed >> 32));
    }
    else { in.sin_family = AF_UNIX; }
    socklen_t real = have ? (socklen_t)sizeof in : 2U;
    socklen_t n = *len < real ? *len : real;
    memcpy(addr, &in, n);
    *len = real;
}

int accept4(int fd, struct sockaddr* addr, socklen_t* len, int flags)
{
    if (__rio_ep_active) __rio_epoll_rearm(fd);
    uint64_t peer = 0;
    long r = sret(user_syscall3(K_ACCEPT, (uint64_t)fd, (uint64_t)&peer, (uint64_t)flags));
    if (r >= 0) fill_in(addr, len, peer, peer != 0);
    return (int)r;
}
int accept(int fd, struct sockaddr* addr, socklen_t* len) { return accept4(fd, addr, len, 0); }

/* wait until fd is ready for `ev` within ms (0 = unlimited); returns 1 ready, 0 timeout, -1 error */
static int wait_fd(int fd, short ev, unsigned ms)
{
    struct pollfd p; p.fd = fd; p.events = ev; p.revents = 0;
    int r = poll(&p, 1, ms ? (int)ms : -1);
    return r;
}

static int with_dontwait(int fd, int flags, int* restore)
{
    *restore = -1;
    if (!(flags & MSG_DONTWAIT)) return 0;
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0) return -1;
    if (!(fl & O_NONBLOCK)) { if (fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0) return -1; *restore = fl; }
    return 0;
}

ssize_t sendto(int fd, const void* buf, size_t n, int flags, const struct sockaddr* addr, socklen_t len)
{
    if (__rio_ep_active) __rio_epoll_rearm(fd);
    uint64_t dest = 0; int has = 0;
    if (addr && addr->sa_family == AF_INET && len >= sizeof(struct sockaddr_in))
    {
        const struct sockaddr_in* in = (const struct sockaddr_in*)addr;
        dest = (uint64_t)ntohl(in->sin_addr.s_addr) | ((uint64_t)ntohs(in->sin_port) << 32); has = 1;
    }
    int restore; if (with_dontwait(fd, flags, &restore) < 0) return -1;
    ssize_t total = 0; const char* p = (const char*)buf;
    if (fd >= 0 && fd < MAXFD && snd_to[fd] && !(flags & MSG_DONTWAIT) && wait_fd(fd, POLLOUT, snd_to[fd]) == 0) { errno = EAGAIN; return -1; }
    do {
        size_t chunk = n - (size_t)total; if (chunk > 4096) chunk = 4096;
        long r = sret(user_syscall5(K_SENDTO, (uint64_t)fd, (uint64_t)(p + total), chunk, dest, (uint64_t)has));
        if (r < 0) { if (total) break; total = -1; break; }
        total += r;
        if ((size_t)r < chunk) break;
        if (has) break;                                    /* datagrams are never split */
    } while ((size_t)total < n);
    if (restore >= 0) fcntl(fd, F_SETFL, restore);
    return total;
}
ssize_t send(int fd, const void* buf, size_t n, int flags) { return sendto(fd, buf, n, flags, 0, 0); }

ssize_t recvfrom(int fd, void* buf, size_t n, int flags, struct sockaddr* addr, socklen_t* len)
{
    if (__rio_ep_active) __rio_epoll_rearm(fd);
    int restore; if (with_dontwait(fd, flags, &restore) < 0) return -1;
    if (fd >= 0 && fd < MAXFD && rcv_to[fd] && !(flags & MSG_DONTWAIT))
    {
        int w = wait_fd(fd, POLLIN, rcv_to[fd]);
        if (w == 0) { errno = EAGAIN; if (restore >= 0) fcntl(fd, F_SETFL, restore); return -1; }
    }
    if (n > 4096) n = 4096;
    uint64_t peer = 0;
    long r;
    for (;;)
    {
        r = sret(user_syscall4(K_RECVFROM, (uint64_t)fd, (uint64_t)buf, n, (uint64_t)&peer));
        if (r < 0 && errno == EINTR && __rio_sa_restart) { __rio_sa_restart = 0; continue; }
        break;
    }
    if (restore >= 0) fcntl(fd, F_SETFL, restore);
    if (r >= 0 && addr && len) fill_in(addr, len, peer, peer != 0);
    return r;
}
ssize_t recv(int fd, void* buf, size_t n, int flags) { return recvfrom(fd, buf, n, flags, 0, 0); }

int shutdown(int fd, int how) { return (int)sret(user_syscall2(K_SHUTDOWN, (uint64_t)fd, (uint64_t)how)); }

static int sockname(int fd, int peer, struct sockaddr* addr, socklen_t* len)
{
    uint64_t packed = 0;
    long r = sret(user_syscall3(K_SOCKNAME, (uint64_t)fd, (uint64_t)peer, (uint64_t)&packed));
    if (r < 0 && errno == ENOTSOCK) return -1;
    if (r < 0) return -1;
    fill_in(addr, len, packed, 1);
    return 0;
}
int getsockname(int fd, struct sockaddr* addr, socklen_t* len) { return sockname(fd, 0, addr, len); }
int getpeername(int fd, struct sockaddr* addr, socklen_t* len) { return sockname(fd, 1, addr, len); }

int setsockopt(int fd, int level, int name, const void* val, socklen_t len)
{
    if (sret(user_syscall2(K_SOCKINFO, (uint64_t)fd, 1)) < 0) return -1;          /* not a socket / bad fd */
    if (level == SOL_SOCKET && (name == SO_RCVTIMEO || name == SO_SNDTIMEO))
    {
        if (!val || len < sizeof(struct timeval)) { errno = EINVAL; return -1; }
        const struct timeval* tv = (const struct timeval*)val;
        unsigned ms = (unsigned)(tv->tv_sec * 1000 + tv->tv_usec / 1000);
        if (fd >= 0 && fd < MAXFD) { if (name == SO_RCVTIMEO) rcv_to[fd] = ms; else snd_to[fd] = ms; }
        return 0;
    }
    (void)val; (void)len;
    return 0;        /* SO_REUSEADDR, TCP_NODELAY, buffer sizes, keepalive ...: accepted, no effect */
}
int getsockopt(int fd, int level, int name, void* val, socklen_t* len)
{
    if (!val || !len || *len < sizeof(int)) { if (val && len && level == SOL_SOCKET && (name == SO_RCVTIMEO || name == SO_SNDTIMEO)) goto tmo; errno = EINVAL; return -1; }
    if (level == SOL_SOCKET && name == SO_ERROR)
    {
        long r = sret(user_syscall2(K_SOCKINFO, (uint64_t)fd, 0)); if (r < 0) return -1;
        *(int*)val = (int)r; *len = sizeof(int); return 0;
    }
    if (level == SOL_SOCKET && name == SO_TYPE)
    {
        long r = sret(user_syscall2(K_SOCKINFO, (uint64_t)fd, 1)); if (r < 0) return -1;
        *(int*)val = (int)r; *len = sizeof(int); return 0;
    }
    if (level == SOL_SOCKET && (name == SO_RCVTIMEO || name == SO_SNDTIMEO)) goto tmo;
    if (sret(user_syscall2(K_SOCKINFO, (uint64_t)fd, 1)) < 0) return -1;
    *(int*)val = (name == SO_RCVBUF || name == SO_SNDBUF) ? 4096 : 0; *len = sizeof(int);
    return 0;
tmo:
    {
        if (*len < sizeof(struct timeval)) { errno = EINVAL; return -1; }
        unsigned ms = fd >= 0 && fd < MAXFD ? (name == SO_RCVTIMEO ? rcv_to[fd] : snd_to[fd]) : 0;
        struct timeval* tv = (struct timeval*)val; tv->tv_sec = ms / 1000; tv->tv_usec = (long)(ms % 1000) * 1000; *len = sizeof *tv;
        return 0;
    }
}

/* ---- sendmsg / recvmsg (scatter-gather + SCM_RIGHTS) ------------------------------------------------------------ */
struct cmsghdr* CMSG_NXTHDR(struct msghdr* m, struct cmsghdr* c)
{
    if (!c) return CMSG_FIRSTHDR(m);
    if (c->cmsg_len < sizeof(struct cmsghdr)) return 0;
    unsigned char* next = (unsigned char*)c + CMSG_ALIGN(c->cmsg_len);
    if (next + sizeof(struct cmsghdr) > (unsigned char*)m->msg_control + m->msg_controllen) return 0;
    return (struct cmsghdr*)next;
}

ssize_t sendmsg(int fd, const struct msghdr* msg, int flags)
{
    if (!msg) { errno = EINVAL; return -1; }
    char tmp[4096]; size_t total = 0;
    for (size_t i = 0; i < msg->msg_iovlen; ++i)
    {
        size_t l = msg->msg_iov[i].iov_len;
        if (total + l > sizeof tmp) l = sizeof tmp - total;
        memcpy(tmp + total, msg->msg_iov[i].iov_base, l); total += l;
        if (total == sizeof tmp) break;
    }
    int fds[8]; int nfds = 0;
    for (struct cmsghdr* c = CMSG_FIRSTHDR((struct msghdr*)msg); c; c = CMSG_NXTHDR((struct msghdr*)msg, c))
    {
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS)
        {
            int cnt = (int)((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
            for (int k = 0; k < cnt && nfds < 8; ++k) fds[nfds++] = ((int*)CMSG_DATA(c))[k];
        }
    }
    if (nfds == 0) return sendto(fd, tmp, total, flags, (const struct sockaddr*)msg->msg_name, msg->msg_namelen);
    int restore; if (with_dontwait(fd, flags, &restore) < 0) return -1;
    long r = sret(user_syscall5(K_SENDMSG, (uint64_t)fd, (uint64_t)tmp, total, (uint64_t)fds, (uint64_t)nfds));
    if (restore >= 0) fcntl(fd, F_SETFL, restore);
    return r;
}

ssize_t recvmsg(int fd, struct msghdr* msg, int flags)
{
    if (!msg) { errno = EINVAL; return -1; }
    char tmp[4096]; size_t cap = 0;
    for (size_t i = 0; i < msg->msg_iovlen; ++i) { cap += msg->msg_iov[i].iov_len; if (cap >= sizeof tmp) { cap = sizeof tmp; break; } }
    int maxfds = 0;
    if (msg->msg_control && msg->msg_controllen >= CMSG_SPACE(sizeof(int)))
        maxfds = (int)((msg->msg_controllen - CMSG_LEN(0)) / sizeof(int));
    if (maxfds > 8) maxfds = 8;
    int fds[8]; int got = 0;
    int restore; if (with_dontwait(fd, flags, &restore) < 0) return -1;
    long r = sret(user_syscall6(K_RECVMSG, (uint64_t)fd, (uint64_t)tmp, cap, (uint64_t)fds, (uint64_t)maxfds, (uint64_t)&got));
    if (restore >= 0) fcntl(fd, F_SETFL, restore);
    if (r < 0) return -1;
    size_t left = (size_t)r, off = 0;
    for (size_t i = 0; i < msg->msg_iovlen && left; ++i)
    {
        size_t l = msg->msg_iov[i].iov_len < left ? msg->msg_iov[i].iov_len : left;
        memcpy(msg->msg_iov[i].iov_base, tmp + off, l); off += l; left -= l;
    }
    msg->msg_flags = 0;
    if (got > 0 && maxfds > 0)
    {
        struct cmsghdr* c = (struct cmsghdr*)msg->msg_control;
        c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS; c->cmsg_len = CMSG_LEN((size_t)got * sizeof(int));
        memcpy(CMSG_DATA(c), fds, (size_t)got * sizeof(int));
        msg->msg_controllen = CMSG_SPACE((size_t)got * sizeof(int));
    }
    else msg->msg_controllen = 0;
    return r;
}

/* ---- pipe2, readv/writev ---------------------------------------------------------------------------------------- */
int pipe2(int fds[2], int flags) { return (int)sret(user_syscall2(K_PIPE2, (uint64_t)fds, (uint64_t)flags)); }
ssize_t readv(int fd, const struct iovec* iov, int cnt)
{
    ssize_t total = 0;
    for (int i = 0; i < cnt; ++i)
    {
        if (iov[i].iov_len == 0) continue;
        ssize_t r = read(fd, iov[i].iov_base, iov[i].iov_len);
        if (r < 0) return total ? total : -1;
        total += r;
        if ((size_t)r < iov[i].iov_len) break;
    }
    return total;
}
ssize_t writev(int fd, const struct iovec* iov, int cnt)
{
    ssize_t total = 0;
    for (int i = 0; i < cnt; ++i)
    {
        if (iov[i].iov_len == 0) continue;
        ssize_t r = write(fd, iov[i].iov_base, iov[i].iov_len);
        if (r < 0) return total ? total : -1;
        total += r;
        if ((size_t)r < iov[i].iov_len) break;
    }
    return total;
}

/* ---- poll / select ---------------------------------------------------------------------------------------------- */
int poll(struct pollfd* fds, nfds_t n, int timeout_ms)
{
    if (n > 64) { errno = EINVAL; return -1; }
    if (!__rio_threads_started)
        return (int)sret(user_syscall3(K_POLL, (uint64_t)fds, (uint64_t)n, (uint64_t)(long)timeout_ms));
    /* other green threads must keep running while this one waits: poll without blocking, sleep in between */
    unsigned long deadline = timeout_ms > 0 ? clock_ms() + (unsigned long)timeout_ms : 0;
    for (;;)
    {
        long r = sret(user_syscall3(K_POLL, (uint64_t)fds, (uint64_t)n, 0));
        if (r != 0) return (int)r;
        if (timeout_ms == 0) return 0;
        if (timeout_ms > 0 && clock_ms() >= deadline) return 0;
        usleep(1000);
    }
}
int ppoll(struct pollfd* fds, nfds_t n, const struct timespec* timeout, const void* sigmask)
{
    (void)sigmask;
    int ms = -1;
    if (timeout) ms = (int)(timeout->tv_sec * 1000 + (timeout->tv_nsec + 999999) / 1000000);
    return poll(fds, n, ms);
}

int select(int nfds, fd_set* r, fd_set* w, fd_set* e, struct timeval* tv)
{
    if (nfds < 0 || nfds > FD_SETSIZE) { errno = EINVAL; return -1; }
    struct pollfd p[FD_SETSIZE]; int n = 0;
    for (int fd = 0; fd < nfds; ++fd)
    {
        short ev = 0;
        if (r && FD_ISSET(fd, r)) ev |= POLLIN;
        if (w && FD_ISSET(fd, w)) ev |= POLLOUT;
        if (e && FD_ISSET(fd, e)) ev |= POLLPRI;
        if (!ev) continue;
        p[n].fd = fd; p[n].events = ev; p[n].revents = 0; ++n;
    }
    int ms = -1;
    if (tv) ms = (int)(tv->tv_sec * 1000 + (tv->tv_usec + 999) / 1000);
    int rc = poll(p, (nfds_t)n, ms);
    if (rc < 0) return -1;
    if (r) FD_ZERO(r); if (w) FD_ZERO(w); if (e) FD_ZERO(e);
    int count = 0;
    for (int i = 0; i < n; ++i)
    {
        short re = p[i].revents; int hit = 0;
        if (re & POLLNVAL) { errno = EBADF; return -1; }
        if (r && (p[i].events & POLLIN) && (re & (POLLIN | POLLHUP | POLLERR))) { FD_SET(p[i].fd, r); ++hit; }
        if (w && (p[i].events & POLLOUT) && (re & (POLLOUT | POLLERR))) { FD_SET(p[i].fd, w); ++hit; }
        if (e && (re & POLLPRI)) { FD_SET(p[i].fd, e); ++hit; }
        count += hit;
    }
    if (tv && rc == 0) { tv->tv_sec = 0; tv->tv_usec = 0; }
    return count;
}
int pselect(int nfds, fd_set* r, fd_set* w, fd_set* e, const struct timespec* ts, const void* sigmask)
{
    (void)sigmask;
    struct timeval tv; struct timeval* ptv = 0;
    if (ts) { tv.tv_sec = ts->tv_sec; tv.tv_usec = ts->tv_nsec / 1000; ptv = &tv; }
    return select(nfds, r, w, e, ptv);
}

/* ---- eventfd ---------------------------------------------------------------------------------------------------- */
int eventfd(unsigned initval, int flags) { return (int)sret(user_syscall2(K_EVENTFD, initval, (uint64_t)flags)); }
int eventfd_read(int fd, eventfd_t* value) { return read(fd, value, 8) == 8 ? 0 : -1; }
int eventfd_write(int fd, eventfd_t value) { return write(fd, &value, 8) == 8 ? 0 : -1; }

/* ---- name resolution --------------------------------------------------------------------------------------------- */
static int service_port(const char* s, int* port)
{
    if (!s || !*s) { *port = 0; return 0; }
    if (*s >= '0' && *s <= '9') { long v = 0; while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); if (v > 65535) return -1; ++s; } if (*s) return -1; *port = (int)v; return 0; }
    static const struct { const char* n; int p; } tab[] = { {"http", 80}, {"https", 443}, {"ftp", 21}, {"ssh", 22}, {"telnet", 23}, {"smtp", 25},
        {"domain", 53}, {"dns", 53}, {"pop3", 110}, {"ntp", 123}, {"imap", 143}, {"imaps", 993}, {"echo", 7} };
    for (unsigned i = 0; i < sizeof tab / sizeof tab[0]; ++i) if (strcmp(s, tab[i].n) == 0) { *port = tab[i].p; return 0; }
    return -1;
}

static int resolve_v4(const char* name, uint32_t* host)
{
    if (parse_v4(name, host)) return 0;
    if (strcmp(name, "localhost") == 0) { *host = INADDR_LOOPBACK; return 0; }
    uint32_t ip = 0;
    if (user_dns_lookup(name, &ip, 5000) != 0) return -1;
    *host = ip; return 0;
}

int getaddrinfo(const char* node, const char* service, const struct addrinfo* hints, struct addrinfo** res)
{
    int family = hints ? hints->ai_family : AF_UNSPEC, socktype = hints ? hints->ai_socktype : 0, flags = hints ? hints->ai_flags : 0;
    if (family != AF_UNSPEC && family != AF_INET) return EAI_FAMILY;
    if (socktype != 0 && socktype != SOCK_STREAM && socktype != SOCK_DGRAM) return EAI_SOCKTYPE;
    int port = 0;
    if (service_port(service, &port) != 0) return EAI_SERVICE;
    uint32_t host;
    if (!node) host = (flags & AI_PASSIVE) ? INADDR_ANY : INADDR_LOOPBACK;
    else if (flags & AI_NUMERICHOST) { if (!parse_v4(node, &host)) return EAI_NONAME; }
    else if (resolve_v4(node, &host) != 0) return EAI_NONAME;
    if (!node && !service) return EAI_NONAME;
    int types[2]; int nt = 0;
    if (socktype) types[nt++] = socktype; else { types[nt++] = SOCK_STREAM; types[nt++] = SOCK_DGRAM; }
    struct addrinfo* head = 0; struct addrinfo** tail = &head;
    for (int i = 0; i < nt; ++i)
    {
        struct addrinfo* ai = (struct addrinfo*)calloc(1, sizeof(struct addrinfo) + sizeof(struct sockaddr_in));
        if (!ai) { freeaddrinfo(head); return EAI_MEMORY; }
        struct sockaddr_in* in = (struct sockaddr_in*)(ai + 1);
        in->sin_family = AF_INET; in->sin_port = htons((uint16_t)port); in->sin_addr.s_addr = htonl(host);
        ai->ai_family = AF_INET; ai->ai_socktype = types[i]; ai->ai_protocol = types[i] == SOCK_STREAM ? IPPROTO_TCP : IPPROTO_UDP;
        ai->ai_addrlen = sizeof *in; ai->ai_addr = (struct sockaddr*)in;
        if ((flags & AI_CANONNAME) && node && i == 0) ai->ai_canonname = strdup(node);
        *tail = ai; tail = &ai->ai_next;
    }
    *res = head;
    return 0;
}
void freeaddrinfo(struct addrinfo* ai) { while (ai) { struct addrinfo* n = ai->ai_next; free(ai->ai_canonname); free(ai); ai = n; } }
const char* gai_strerror(int code)
{
    switch (code) { case 0: return "success"; case EAI_NONAME: return "name or service not known"; case EAI_AGAIN: return "temporary failure";
        case EAI_FAIL: return "non-recoverable failure"; case EAI_FAMILY: return "address family not supported"; case EAI_SOCKTYPE: return "socket type not supported";
        case EAI_SERVICE: return "service not supported"; case EAI_MEMORY: return "out of memory"; default: return "unknown error"; }
}
struct hostent* gethostbyname(const char* name)
{
    static struct hostent he; static char* addrs[2]; static char* aliases[1]; static struct in_addr a; static char hname[256];
    uint32_t host; if (!name || resolve_v4(name, &host) != 0) return 0;
    a.s_addr = htonl(host); addrs[0] = (char*)&a; addrs[1] = 0; aliases[0] = 0;
    strncpy(hname, name, sizeof hname - 1); hname[sizeof hname - 1] = 0;
    he.h_name = hname; he.h_aliases = aliases; he.h_addrtype = AF_INET; he.h_length = 4; he.h_addr_list = addrs;
    return &he;
}
int getnameinfo(const struct sockaddr* sa, socklen_t salen, char* host, socklen_t hostlen, char* serv, socklen_t servlen, int flags)
{
    (void)flags;
    if (!sa || sa->sa_family != AF_INET || salen < sizeof(struct sockaddr_in)) return EAI_FAMILY;
    const struct sockaddr_in* in = (const struct sockaddr_in*)sa;
    if (host && hostlen >= INET_ADDRSTRLEN) fmt_v4(ntohl(in->sin_addr.s_addr), host); else if (host) return EAI_MEMORY;
    if (serv)
    {
        char t[8]; unsigned v = ntohs(in->sin_port); int n = 0; char r[8];
        if (v == 0) r[n++] = '0';
        while (v) { r[n++] = (char)('0' + v % 10); v /= 10; }
        for (int i = 0; i < n; ++i) t[i] = r[n - 1 - i];
        t[n] = 0;
        if (servlen <= (socklen_t)n) return EAI_MEMORY;
        strcpy(serv, t);
    }
    return 0;
}
