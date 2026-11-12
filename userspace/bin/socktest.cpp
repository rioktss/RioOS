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
/* socktest: BSD sockets over the real network (driven by tests/qemu_net_tests.py).
   usage: socktest client <hostip> <port> | udp <hostip> <port> | server <port> | loop <port> */
static int fails = 0;
static void chk(const char* name, int ok) { printf("%s %s\n", ok ? "PASS" : "FAIL", name); if (!ok) ++fails; }
static int mk_addr(struct sockaddr_in* a, const char* ip, int port)
{ memset(a, 0, sizeof *a); a->sin_family = AF_INET; a->sin_port = htons((uint16_t)port); return inet_pton(AF_INET, ip, &a->sin_addr); }

static int do_client(const char* ip, int port)
{
    int s = socket(AF_INET, SOCK_STREAM, 0); chk("tcp-socket", s >= 0);
    struct sockaddr_in a; mk_addr(&a, ip, port);
    chk("tcp-connect", connect(s, (struct sockaddr*)&a, sizeof a) == 0);
    struct sockaddr_in peer; socklen_t pl = sizeof peer;
    chk("getpeername", getpeername(s, (struct sockaddr*)&peer, &pl) == 0 && ntohs(peer.sin_port) == port && peer.sin_addr.s_addr == a.sin_addr.s_addr);
    struct sockaddr_in me; pl = sizeof me; chk("getsockname-ephemeral", getsockname(s, (struct sockaddr*)&me, &pl) == 0 && ntohs(me.sin_port) >= 1024);
    int err = -1; socklen_t el = sizeof err; chk("so-error-zero", getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &el) == 0 && err == 0);
    struct pollfd p; p.fd = s; p.events = POLLOUT; p.revents = 0; chk("poll-connected-writable", poll(&p, 1, 0) == 1 && (p.revents & POLLOUT));
    p.events = POLLIN; p.revents = 0; chk("poll-idle-not-readable", poll(&p, 1, 0) == 0);
    chk("send", send(s, "hello ", 6, 0) == 6);
    chk("send2", send(s, "world", 5, 0) == 5);
    chk("shutdown-wr", shutdown(s, SHUT_WR) == 0);
    errno = 0; chk("send-after-shutdown-epipe", ({ signal(SIGPIPE, SIG_IGN); send(s, "x", 1, 0) == -1 && errno == EPIPE; }));
    char buf[256]; int n = 0, total = 0;
    p.events = POLLIN; p.revents = 0;
    chk("poll-response-arrives", poll(&p, 1, 8000) == 1 && (p.revents & POLLIN));
    while ((n = (int)recv(s, buf + total, sizeof buf - 1 - total, 0)) > 0) total += n;
    buf[total] = 0;
    chk("recv-eof-after-half-close", n == 0);
    printf("RESPONSE[%s]\n", buf);
    chk("response-content", strcmp(buf, "got:hello world") == 0);
    chk("poll-eof-hup", ({ struct pollfd q; q.fd = s; q.events = POLLIN; q.revents = 0; poll(&q, 1, 1000) == 1 && (q.revents & (POLLIN | POLLHUP)); }));
    close(s);
    /* connection refused */
    s = socket(AF_INET, SOCK_STREAM, 0); mk_addr(&a, ip, 1); errno = 0;
    chk("connect-refused", connect(s, (struct sockaddr*)&a, sizeof a) == -1 && (errno == ECONNREFUSED)); close(s);
    return 0;
}

static int do_udp(const char* ip, int port)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0); chk("udp-socket", s >= 0);
    struct sockaddr_in a; mk_addr(&a, ip, port);
    chk("udp-sendto", sendto(s, "ping-1", 6, 0, (struct sockaddr*)&a, sizeof a) == 6);
    struct pollfd p; p.fd = s; p.events = POLLIN; p.revents = 0;
    chk("udp-poll-reply", poll(&p, 1, 5000) == 1);
    char buf[64] = {0}; struct sockaddr_in from; socklen_t fl = sizeof from;
    ssize_t n = recvfrom(s, buf, sizeof buf - 1, 0, (struct sockaddr*)&from, &fl);
    chk("udp-recvfrom", n == 6 && memcmp(buf, "echo:1", 6) == 0 && ntohs(from.sin_port) == port && from.sin_addr.s_addr == a.sin_addr.s_addr);
    chk("udp-connect", connect(s, (struct sockaddr*)&a, sizeof a) == 0);
    chk("udp-send-connected", send(s, "ping-2", 6, 0) == 6);
    memset(buf, 0, sizeof buf); n = recv(s, buf, sizeof buf - 1, 0);
    chk("udp-recv-connected", n == 6 && memcmp(buf, "echo:2", 6) == 0);
    close(s);
    return 0;
}

static int do_server(int port)
{
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1; setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a; memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port);
    chk("srv-bind", bind(ls, (struct sockaddr*)&a, sizeof a) == 0);
    chk("srv-listen", listen(ls, 4) == 0);
    chk("srv-idle-not-readable", ({ struct pollfd p; p.fd = ls; p.events = POLLIN; p.revents = 0; poll(&p, 1, 0) == 0; }));
    printf("LISTENING\n");
    struct pollfd p; p.fd = ls; p.events = POLLIN; p.revents = 0;
    chk("srv-poll-accept-ready", poll(&p, 1, 120000) == 1 && (p.revents & POLLIN));
    int c = -1; char buf[128]; int total = 0, n = 0;
    for (int attempt = 0; attempt < 4 && total == 0; ++attempt)          /* the test harness probes the port once with a connect+close */
    {
        struct sockaddr_in peer; socklen_t pl = sizeof peer;
        if (c >= 0) close(c);
        p.revents = 0; (void)poll(&p, 1, 60000);
        c = accept(ls, (struct sockaddr*)&peer, &pl);
        if (attempt == 0) chk("srv-accept", c >= 0 && pl == sizeof peer && peer.sin_addr.s_addr != 0);
        if (c < 0) continue;
        while ((n = (int)recv(c, buf + total, sizeof buf - 1 - total, 0)) > 0) { total += n; if (total >= 12) break; }
    }
    buf[total] = 0; printf("SERVER-GOT[%s]\n", buf);
    chk("srv-recv", strcmp(buf, "from-client!") == 0);
    char out[160]; snprintf(out, sizeof out, "echo:%s", buf);
    chk("srv-send", send(c, out, strlen(out), 0) == (ssize_t)strlen(out));
    close(c); close(ls);
    return 0;
}

/* event loop: accept several clients with poll(), echo each line back (non-blocking sockets) */
static int do_loop(int port)
{
    int ls = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    struct sockaddr_in a; memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port);
    chk("loop-bind", bind(ls, (struct sockaddr*)&a, sizeof a) == 0);
    chk("loop-listen", listen(ls, 8) == 0);
    errno = 0; chk("loop-accept-nonblock-eagain", accept(ls, 0, 0) == -1 && errno == EAGAIN);
    printf("LISTENING\n");
    struct pollfd fds[8]; int nfds = 1; fds[0].fd = ls; fds[0].events = POLLIN;
    int served = 0, closed = 0;
    unsigned long end = clock_ms() + 120000;
    while (!(served >= 3 && closed >= 3) && clock_ms() < end)
    {
        for (int i = 0; i < nfds; ++i) fds[i].revents = 0;
        int r = poll(fds, (nfds_t)nfds, 2000);
        if (r <= 0) continue;
        if (fds[0].revents & POLLIN)
        {
            int c = accept(ls, 0, 0);
            if (c >= 0 && nfds < 8) { fcntl(c, F_SETFL, fcntl(c, F_GETFL) | O_NONBLOCK); fds[nfds].fd = c; fds[nfds].events = POLLIN; ++nfds; }
        }
        for (int i = 1; i < nfds; ++i)
        {
            if (!(fds[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            char b[256]; ssize_t n = recv(fds[i].fd, b, sizeof b, 0);
            if (n > 0) { send(fds[i].fd, b, (size_t)n, 0); ++served; }
            else if (n == 0 || (errno != EAGAIN)) { close(fds[i].fd); fds[i] = fds[nfds - 1]; --nfds; --i; ++closed; }
        }
    }
    chk("loop-served-clients", closed >= 3 && served >= 3);
    printf("LOOP served=%d closed=%d\n", served, closed);
    close(ls);
    return 0;
}

int main(int argc, char** argv)
{
    if (argc < 3) { printf("usage: socktest client|udp <ip> <port> | server|loop <port>\n"); return 2; }
    if (strcmp(argv[1], "client") == 0 && argc >= 4) do_client(argv[2], atoi(argv[3]));
    else if (strcmp(argv[1], "udp") == 0 && argc >= 4) do_udp(argv[2], atoi(argv[3]));
    else if (strcmp(argv[1], "server") == 0) do_server(atoi(argv[2]));
    else if (strcmp(argv[1], "loop") == 0) do_loop(atoi(argv[2]));
    else { printf("bad args\n"); return 2; }
    printf("socktest done fails=%d\n", fails);
    return fails ? 1 : 0;
}
