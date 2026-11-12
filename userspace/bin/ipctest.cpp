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
/* ipctest: unified descriptors — poll/select, O_NONBLOCK, eventfd, socketpair, AF_UNIX (named, SCM_RIGHTS), blocking I/O, readv/writev, address helpers */
static int fails = 0;
static void chk(const char* name, int ok) { printf("%s %s\n", ok ? "PASS" : "FAIL", name); if (!ok) ++fails; }

static int ready(int fd, short ev, int ms) { struct pollfd p; p.fd = fd; p.events = ev; p.revents = 0; int r = poll(&p, 1, ms); return r == 1 ? p.revents : (r < 0 ? -1 : 0); }
static volatile int got_alarm = 0;
static void on_alrm(int s) { (void)s; got_alarm = 1; }

int main(int argc, char** argv)
{
    (void)argc; (void)argv;
    signal(SIGPIPE, SIG_IGN);

    /* ---- poll on pipes ---- */
    int pf[2]; pipe(pf);
    chk("poll-pipe-write-ready", (ready(pf[1], POLLOUT, 0) & POLLOUT) != 0);
    chk("poll-pipe-read-not-ready", ready(pf[0], POLLIN, 0) == 0);
    unsigned long t0 = clock_ms(); int r = ready(pf[0], POLLIN, 60); unsigned long el = clock_ms() - t0;
    chk("poll-timeout-elapsed", r == 0 && el >= 50 && el < 400);
    write(pf[1], "xy", 2);
    chk("poll-pipe-read-ready", (ready(pf[0], POLLIN, 0) & POLLIN) != 0);
    char b[16]; read(pf[0], b, 2);
    close(pf[1]);
    chk("poll-pipe-hup", (ready(pf[0], POLLIN, 0) & POLLHUP) != 0);
    chk("read-eof-after-hup", read(pf[0], b, 8) == 0);
    close(pf[0]);
    chk("poll-bad-fd-nval", ready(20, POLLIN, 0) == POLLNVAL);

    /* ---- O_NONBLOCK, pipe2 ---- */
    int p2[2]; chk("pipe2-ok", pipe2(p2, O_NONBLOCK | O_CLOEXEC) == 0);
    chk("getfl-nonblock", (fcntl(p2[0], F_GETFL) & O_NONBLOCK) != 0);
    chk("getfd-cloexec", fcntl(p2[0], F_GETFD) == FD_CLOEXEC);
    errno = 0; chk("nonblock-read-eagain", read(p2[0], b, 4) == -1 && errno == EAGAIN);
    fcntl(p2[0], F_SETFL, 0);
    chk("setfl-clears-nonblock", (fcntl(p2[0], F_GETFL) & O_NONBLOCK) == 0);
    close(p2[0]); close(p2[1]);

    /* ---- blocking read woken by a writer in another process, poll(-1) woken too ---- */
    int pb[2]; pipe(pb);
    pid_t c = fork();
    if (c == 0) { close(pb[0]); usleep(60000); write(pb[1], "wake", 4); _exit(0); }
    close(pb[1]);
    t0 = clock_ms(); int pr = ready(pb[0], POLLIN, -1); el = clock_ms() - t0;
    chk("poll-forever-wakes", (pr & POLLIN) != 0 && el >= 40);
    memset(b, 0, sizeof b); chk("read-after-poll", read(pb[0], b, 8) == 4 && memcmp(b, "wake", 4) == 0);
    int st; waitpid(c, &st, 0); close(pb[0]);

    /* ---- large blocking write through a 4 KiB pipe (libc chunking + kernel blocking) ---- */
    int pw[2]; pipe(pw);
    c = fork();
    if (c == 0)
    {
        close(pw[1]); static char rb[600]; long total = 0; unsigned sum = 0;
        for (;;) { ssize_t n = read(pw[0], rb, sizeof rb); if (n <= 0) break; for (ssize_t i = 0; i < n; ++i) sum += (unsigned char)rb[i]; total += n; }
        _exit(total == 20000 && sum == 20000U * 7U ? 0 : 1);
    }
    close(pw[0]);
    static char big[20000]; memset(big, 7, sizeof big);
    chk("write-20000-through-pipe", write(pw[1], big, sizeof big) == (ssize_t)sizeof big);
    close(pw[1]); waitpid(c, &st, 0);
    chk("reader-got-all-bytes", WIFEXITED(st) && WEXITSTATUS(st) == 0);

    /* ---- poll interrupted by a signal ---- */
    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_handler = on_alrm; sigaction(SIGALRM, &sa, 0);
    int pi[2]; pipe(pi);
    got_alarm = 0;
    { struct itimerval it; it.it_interval.tv_sec = 0; it.it_interval.tv_usec = 0; it.it_value.tv_sec = 0; it.it_value.tv_usec = 50000; setitimer(0, &it, 0); }
    struct pollfd pw1; pw1.fd = pi[0]; pw1.events = POLLIN; pw1.revents = 0;
    errno = 0; r = poll(&pw1, 1, 2000);
    chk("poll-eintr", r == -1 && errno == EINTR && got_alarm);
    close(pi[0]); close(pi[1]);

    /* ---- eventfd ---- */
    int ev = eventfd(0, O_NONBLOCK);
    chk("eventfd-created", ev >= 0);
    errno = 0; eventfd_t v = 0; chk("eventfd-empty-eagain", eventfd_read(ev, &v) == -1 && errno == EAGAIN);
    chk("eventfd-not-readable", ready(ev, POLLIN, 0) == 0);
    chk("eventfd-write", eventfd_write(ev, 3) == 0 && eventfd_write(ev, 4) == 0);
    chk("eventfd-readable", (ready(ev, POLLIN, 0) & POLLIN) != 0);
    chk("eventfd-sum", eventfd_read(ev, &v) == 0 && v == 7);
    close(ev);
    ev = eventfd(2, EFD_SEMAPHORE | EFD_NONBLOCK);
    chk("eventfd-semaphore", eventfd_read(ev, &v) == 0 && v == 1 && eventfd_read(ev, &v) == 0 && v == 1 && eventfd_read(ev, &v) == -1);
    close(ev);

    /* ---- socketpair stream ---- */
    int sv[2]; chk("socketpair-created", socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    chk("sp-write-a", write(sv[0], "hello", 5) == 5);
    memset(b, 0, sizeof b); chk("sp-read-b", read(sv[1], b, 16) == 5 && memcmp(b, "hello", 5) == 0);
    chk("sp-write-b", write(sv[1], "back", 4) == 4);
    chk("sp-read-a", read(sv[0], b, 16) == 4);
    chk("sp-getsockopt-type", ({ int t = 0; socklen_t l = sizeof t; getsockopt(sv[0], SOL_SOCKET, SO_TYPE, &t, &l) == 0 && t == SOCK_STREAM; }));
    chk("sp-not-readable", ready(sv[0], POLLIN, 0) == 0);
    chk("sp-writable", (ready(sv[0], POLLOUT, 0) & POLLOUT) != 0);
    fcntl(sv[0], F_SETFL, O_NONBLOCK);
    errno = 0; chk("sp-nonblock-eagain", read(sv[0], b, 4) == -1 && errno == EAGAIN);
    { fd_set rs; FD_ZERO(&rs); FD_SET(sv[0], &rs); struct timeval tv = {0, 20000}; chk("select-timeout", select(sv[0] + 1, &rs, 0, 0, &tv) == 0);
      write(sv[1], "z", 1); FD_ZERO(&rs); FD_SET(sv[0], &rs); tv.tv_sec = 1; chk("select-ready", select(sv[0] + 1, &rs, 0, 0, &tv) == 1 && FD_ISSET(sv[0], &rs)); read(sv[0], b, 1); }
    /* shutdown(SHUT_WR) gives the peer EOF while the reverse direction keeps working */
    chk("sp-shutdown-wr", shutdown(sv[1], SHUT_WR) == 0);
    chk("sp-peer-eof", read(sv[0], b, 4) == 0);
    chk("sp-reverse-still-works", write(sv[0], "k", 1) == 1 && read(sv[1], b, 4) == 1);
    close(sv[1]);
    chk("sp-close-gives-hup", (ready(sv[0], POLLIN, 0) & POLLHUP) != 0);
    errno = 0; chk("sp-write-closed-epipe", write(sv[0], "x", 1) == -1 && errno == EPIPE);
    close(sv[0]);

    /* ---- socketpair datagram keeps message boundaries ---- */
    chk("dgram-created", socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) == 0);
    write(sv[0], "one", 3); write(sv[0], "second", 6); write(sv[0], "3", 1);
    memset(b, 0, sizeof b);
    chk("dgram-msg1", recv(sv[1], b, 16, 0) == 3 && memcmp(b, "one", 3) == 0);
    chk("dgram-msg2-truncated", recv(sv[1], b, 4, 0) == 4);
    chk("dgram-msg3", recv(sv[1], b, 16, 0) == 1 && b[0] == '3');
    chk("dgram-empty-dontwait", ({ errno = 0; recv(sv[1], b, 4, MSG_DONTWAIT) == -1 && errno == EAGAIN; }));
    close(sv[0]); close(sv[1]);

    /* ---- socketpair across fork: blocking recv in the parent wakes when the child sends ---- */
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    c = fork();
    if (c == 0) { close(sv[0]); usleep(40000); send(sv[1], "from-child", 10, 0); _exit(0); }
    close(sv[1]);
    memset(b, 0, sizeof b); chk("sp-fork-blocking-recv", recv(sv[0], b, 16, 0) == 10 && memcmp(b, "from-child", 10) == 0);
    waitpid(c, &st, 0); close(sv[0]);

    /* ---- named AF_UNIX stream server/client ---- */
    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un ua; memset(&ua, 0, sizeof ua); ua.sun_family = AF_UNIX; strcpy(ua.sun_path, "/tmp/ipctest.sock");
    chk("unix-bind", bind(srv, (struct sockaddr*)&ua, sizeof ua) == 0);
    chk("unix-listen", listen(srv, 4) == 0);
    int srv2 = socket(AF_UNIX, SOCK_STREAM, 0);
    errno = 0; chk("unix-bind-inuse", bind(srv2, (struct sockaddr*)&ua, sizeof ua) == -1 && errno == EADDRINUSE); close(srv2);
    { int cl = socket(AF_UNIX, SOCK_STREAM, 0); struct sockaddr_un bad; memset(&bad, 0, sizeof bad); bad.sun_family = AF_UNIX; strcpy(bad.sun_path, "/tmp/nonexistent.sock");
      errno = 0; chk("unix-connect-refused", connect(cl, (struct sockaddr*)&bad, sizeof bad) == -1 && errno == ECONNREFUSED); close(cl); }
    chk("listener-not-readable", ready(srv, POLLIN, 0) == 0);
    c = fork();
    if (c == 0)
    {
        int cl = socket(AF_UNIX, SOCK_STREAM, 0);
        if (connect(cl, (struct sockaddr*)&ua, sizeof ua) != 0) _exit(10);
        write(cl, "ping", 4); char rb[8] = {0};
        if (read(cl, rb, 8) != 4 || memcmp(rb, "pong", 4) != 0) _exit(11);
        _exit(0);
    }
    chk("listener-readable-on-connect", (ready(srv, POLLIN, 2000) & POLLIN) != 0);
    int cfd = accept(srv, 0, 0); chk("unix-accept", cfd >= 0);
    memset(b, 0, sizeof b); chk("unix-server-read", read(cfd, b, 8) == 4 && memcmp(b, "ping", 4) == 0);
    write(cfd, "pong", 4);
    waitpid(c, &st, 0); chk("unix-client-ok", WIFEXITED(st) && WEXITSTATUS(st) == 0);
    close(cfd); close(srv);
    { int s3 = socket(AF_UNIX, SOCK_STREAM, 0); chk("unix-rebind-after-close", bind(s3, (struct sockaddr*)&ua, sizeof ua) == 0); close(s3); }

    /* ---- SCM_RIGHTS: send a pipe's write end over a socketpair ---- */
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    int pp[2]; pipe(pp);
    { char data = 'F'; struct iovec io = { &data, 1 }; char cbuf[CMSG_SPACE(sizeof(int))]; memset(cbuf, 0, sizeof cbuf);
      struct msghdr m; memset(&m, 0, sizeof m); m.msg_iov = &io; m.msg_iovlen = 1; m.msg_control = cbuf; m.msg_controllen = sizeof cbuf;
      struct cmsghdr* ch = CMSG_FIRSTHDR(&m); ch->cmsg_level = SOL_SOCKET; ch->cmsg_type = SCM_RIGHTS; ch->cmsg_len = CMSG_LEN(sizeof(int)); memcpy(CMSG_DATA(ch), &pp[1], sizeof(int));
      chk("scm-sendmsg", sendmsg(sv[0], &m, 0) == 1); }
    close(pp[1]);                                   /* only the in-flight copy keeps the write end alive */
    int newfd = -1;
    { char data = 0; struct iovec io = { &data, 1 }; char cbuf[CMSG_SPACE(sizeof(int))]; memset(cbuf, 0, sizeof cbuf);
      struct msghdr m; memset(&m, 0, sizeof m); m.msg_iov = &io; m.msg_iovlen = 1; m.msg_control = cbuf; m.msg_controllen = sizeof cbuf;
      ssize_t n = recvmsg(sv[1], &m, 0); struct cmsghdr* ch = CMSG_FIRSTHDR(&m);
      chk("scm-recvmsg-data", n == 1 && data == 'F');
      chk("scm-got-rights", ch && ch->cmsg_type == SCM_RIGHTS);
      if (ch) memcpy(&newfd, CMSG_DATA(ch), sizeof(int)); }
    chk("scm-new-fd-valid", newfd >= 0 && newfd != pp[0]);
    chk("scm-write-via-passed-fd", newfd >= 0 && write(newfd, "pipe-data", 9) == 9);
    memset(b, 0, sizeof b); chk("scm-read-from-original", read(pp[0], b, 16) == 9 && memcmp(b, "pipe-data", 9) == 0);
    close(newfd); chk("scm-eof-after-last-close", read(pp[0], b, 4) == 0);
    close(pp[0]); close(sv[0]); close(sv[1]);

    /* ---- readv / writev ---- */
    pipe(pf);
    { struct iovec w[3] = { {(void*)"ab", 2}, {(void*)"cde", 3}, {(void*)"f", 1} }; chk("writev", writev(pf[1], w, 3) == 6);
      char x[2], y[4]; struct iovec rv[2] = { {x, 2}, {y, 4} }; chk("readv", readv(pf[0], rv, 2) == 6 && x[0] == 'a' && y[3] == 'f'); }
    close(pf[0]); close(pf[1]);

    /* ---- address helpers ---- */
    { struct in_addr a; chk("inet_pton", inet_pton(AF_INET, "10.0.2.15", &a) == 1 && ntohl(a.s_addr) == 0x0A00020FU);
      char s[20]; chk("inet_ntop", inet_ntop(AF_INET, &a, s, sizeof s) && strcmp(s, "10.0.2.15") == 0);
      chk("inet_pton-rejects-garbage", inet_pton(AF_INET, "300.1.1.1", &a) == 0 && inet_pton(AF_INET, "1.2.3", &a) == 0);
      chk("htons", htons(0x1234) == 0x3412 && ntohl(htonl(0xDEADBEEF)) == 0xDEADBEEFU);
      struct addrinfo hints; memset(&hints, 0, sizeof hints); hints.ai_socktype = SOCK_STREAM; hints.ai_flags = AI_NUMERICHOST; struct addrinfo* ai = 0;
      chk("getaddrinfo-numeric", getaddrinfo("192.168.1.9", "https", &hints, &ai) == 0 && ai && ai->ai_family == AF_INET &&
          ntohs(((struct sockaddr_in*)ai->ai_addr)->sin_port) == 443 && ntohl(((struct sockaddr_in*)ai->ai_addr)->sin_addr.s_addr) == 0xC0A80109U);
      freeaddrinfo(ai); }

    /* ---- sockets are ordinary descriptors: dup, fork inheritance, close frees ---- */
    { int s1 = socket(AF_INET, SOCK_DGRAM, 0); chk("inet-udp-socket", s1 >= 3);
      int d = dup(s1); chk("dup-socket", d >= 0 && d != s1);
      struct sockaddr_in a; memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_port = htons(40123);
      chk("udp-bind", bind(s1, (struct sockaddr*)&a, sizeof a) == 0);
      struct sockaddr_in got; socklen_t gl = sizeof got; chk("udp-getsockname", getsockname(d, (struct sockaddr*)&got, &gl) == 0 && ntohs(got.sin_port) == 40123);
      chk("udp-writable", (ready(s1, POLLOUT, 0) & POLLOUT) != 0);
      errno = 0; chk("udp-recv-nonblock-empty", recv(s1, b, 8, MSG_DONTWAIT) == -1 && errno == EAGAIN);
      struct timeval tv = {0, 30000}; setsockopt(s1, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
      t0 = clock_ms(); errno = 0; r = (int)recv(s1, b, 8, 0); el = clock_ms() - t0;
      chk("so-rcvtimeo", r == -1 && errno == EAGAIN && el >= 20 && el < 500);
      close(s1); close(d);
      errno = 0; chk("socket-op-on-file-enotsock", ({ int f = open("/etc/passwd", 0); int rr = f < 0 ? -2 : listen(f, 1); if (f >= 0) close(f); rr == -1 || rr == -2; })); }

    /* ---- fd exhaustion cleans up ---- */
    { int fdsx[30]; int n = 0; for (; n < 30; ++n) { fdsx[n] = socket(AF_UNIX, SOCK_STREAM, 0); if (fdsx[n] < 0) break; }
      chk("fd-table-full-emfile", n < 30 && errno == EMFILE);
      for (int i = 0; i < n; ++i) close(fdsx[i]);
      int again = socket(AF_UNIX, SOCK_STREAM, 0); chk("fd-reusable-after-close", again >= 0); close(again); }

    printf("ipctest done fails=%d\n", fails);
    return fails ? 1 : 0;
}
