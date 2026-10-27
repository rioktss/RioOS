#include "syscall.h"
#include "fs.h"
#include "vfs.h"
#include "user.h"
#include "fd.h"
#include "netstack.h"
#include <stdio.h>
#include <string.h>

/* ---- stubs for everything syscall.cpp links against ---- */
static char out[8192]; static unsigned out_len = 0;
void uart_putc(char c) { if (out_len < sizeof(out)) out[out_len++] = c; }
void uart_puts(const char* s) { while (*s) uart_putc(*s++); }
static const char* rx = ""; static unsigned rx_pos = 0;
int uart_try_getc() { return rx[rx_pos] ? (unsigned char)rx[rx_pos++] : -1; }
uint64_t timer_seconds() { return 7; }
uint64_t timer_millis() { return 7123; }
static uint64_t slept = 0;
void timer_delay_ms(uint64_t ms) { slept += ms; }
uint64_t heap_free() { return 4242; }
uint64_t scheduler_ticks() { return 99; }
int scheduler_current_pid() { return 1; }
int user_current_pid() { return 5; }
static int exit_code = -1;
void user_set_exit_code(int c) { exit_code = c; }
extern "C" void user_exit_return() {}

/* Network stubs keep syscall validation tests independent from a device. */
int net_udp_socket(int owner_pid) { return owner_pid >= 0 ? 1 : -22; }
int net_udp_bind(int, int, uint16_t) { return 0; }
int net_udp_sendto(int, int, uint32_t, uint16_t, const void*, uint32_t n) { return (int)n; }
int net_udp_recvfrom(int, int, uint32_t* ip, uint16_t* port, void*, uint32_t, uint32_t)
{ if (ip) *ip = 0x0A000203U; if (port) *port = 53; return -110; }
int net_udp_close(int, int) { return 0; }
void net_udp_close_all_for(int) {}
int net_dns_resolve(const char*, uint32_t* ip, uint32_t) { if (ip) *ip = 0x5DB8D822U; return 0; }
int net_tcp_socket(int owner_pid) { return owner_pid >= 0 ? 2 : -22; }
int net_tcp_connect(int, int, uint32_t, uint16_t, uint32_t) { return 0; }
int net_tcp_send(int, int, const void*, uint32_t length, uint32_t) { return (int)length; }
int net_tcp_recv(int, int, void*, uint32_t, uint32_t) { return -110; }
int net_tcp_close(int, int) { return 0; }
void net_tcp_close_all_for(int) {}

/* Fake single-file filesystem: only "/storage/home/hello" exists after a write. */
static uint8_t fake_file[4096]; static uint64_t fake_size = 0; static int fake_exists = 0;
int fs_get_root() { return 0; }
static int fake_cwd = 7; static int seen_cwd = -1;
int fs_resolve(const char* path, int cwd)
{
    seen_cwd = cwd;
    return path && strcmp(path, "/storage/home/hello") == 0 && fake_exists ? 1 : -1;
}
int fs_touch(const char* path, int cwd)
{
    seen_cwd = cwd;
    if (!path || strcmp(path, "/storage/home/hello") != 0) return -1;
    if (fake_exists) return -2;
    fake_exists = 1; fake_size = 0; return 1;
}
int fs_get_type(int id) { return id == 1 ? FS_FILE : -1; }
uint64_t fs_file_size(int id) { return id == 1 ? fake_size : 0; }
static uint32_t fake_uid = 0, fake_gid = 0, fake_mode = 0644U;
void fs_set_credentials(uint32_t uid, uint32_t gid) { fake_uid = uid; fake_gid = gid; }
uint32_t fs_current_uid() { return fake_uid; }
uint32_t fs_current_gid() { return fake_gid; }
int fs_check_access(int id, uint32_t requested) { (void)requested; return id == 1 ? 0 : -2; }
int fs_stat_path(const char* path, int cwd, FsStat* out)
{
    seen_cwd = cwd;
    if (!path || strcmp(path, "/storage/home/hello") != 0 || !fake_exists) return -2;
    if (!out) return -14;
    out->inode = 1; out->type = FS_FILE; out->mode = fake_mode; out->uid = 1000;
    out->gid = 1000; out->sector_count = (uint32_t)((fake_size + 511U) / 512U);
    out->size = fake_size; out->mtime = 123;
    return 0;
}
int fs_truncate(const char* path, int cwd, uint64_t size)
{
    seen_cwd = cwd;
    if (!path || strcmp(path, "/storage/home/hello") != 0 || !fake_exists) return -2;
    if (size > sizeof fake_file) return -27;
    if (size > fake_size) memset(fake_file + fake_size, 0, (size_t)(size - fake_size));
    fake_size = size; return 0;
}
int fs_set_mode_path(const char* path, int cwd, uint32_t mode)
{
    seen_cwd = cwd;
    if (!path || strcmp(path, "/storage/home/hello") != 0 || !fake_exists) return -2;
    if (mode & ~0777U) return -22;
    fake_mode = mode; return 0;
}
int vfs_cwd() { return fake_cwd; }
static const char* fake_args = "test.txt";
const char* user_get_args() { return fake_args; }
int fs_read_file(const char* path, int cwd, uint8_t* buffer, uint64_t capacity, uint64_t* out_size)
{
    seen_cwd = cwd;
    if (strcmp(path, "/storage/home/hello") != 0 || !fake_exists) return -2;
    if (fake_size > capacity) return -3;
    memcpy(buffer, fake_file, fake_size);
    *out_size = fake_size;
    return 0;
}
int fs_write_data(const char* path, int cwd, const uint8_t* data, uint64_t size)
{
    seen_cwd = cwd;
    if (strcmp(path, "/storage/home/hello") != 0 || data == 0 || size > sizeof fake_file) return -1;
    memcpy(fake_file, data, size); fake_size = size; fake_exists = 1;
    return 0;
}
int fs_read_file_for_update(const char* path, int cwd, uint8_t* buffer,
                            uint64_t capacity, uint64_t* out_size)
{
    /* The standalone syscall test uses a single-file stub. Permission-specific
       update reads are exercised against the real filesystem in fd_host_test. */
    return fs_read_file(path, cwd, buffer, capacity, out_size);
}

/* Fake user window: [0x47000000,0x47001000) read-only, [0x47001000,0x47002000) RW. */
int mmu_user_pointer_ok(uint64_t a, uint64_t len, int write)
{
    if (a == 0 || len == 0) return 0;
    uint64_t end = a + len;
    if (end < a) return 0;
    if (a >= 0x47000000ULL && end <= 0x47001000ULL) return !write;
    if (a >= 0x47001000ULL && end <= 0x47002000ULL) return 1;
    return 0;
}

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (0)

static uint64_t call(uint64_t nr, uint64_t a0 = 0, uint64_t a1 = 0, int user = 1, uint64_t a2 = 0)
{
    ExceptionFrame f; memset(&f, 0, sizeof f);
    f.x[8] = nr; f.x[0] = a0; f.x[1] = a1; f.x[2] = a2;
    syscall_dispatch(&f, user);
    return f.x[0];
}
static uint64_t call6(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2,
                      uint64_t a3, uint64_t a4, uint64_t a5, int user)
{
    ExceptionFrame f; memset(&f, 0, sizeof f);
    f.x[8] = nr; f.x[0] = a0; f.x[1] = a1; f.x[2] = a2;
    f.x[3] = a3; f.x[4] = a4; f.x[5] = a5;
    syscall_dispatch(&f, user);
    return f.x[0];
}
#define ERR(n) ((uint64_t)(-(n)))

int main()
{
    CHECK(call(SYS_PING) == 0x4D594B56ULL);
    CHECK(call(SYS_NET_TCP_SOCKET) == 2);
    CHECK(call6(SYS_NET_TCP_CONNECT, 1, 0x5DB8D822U, 80, 1000, 0, 0, 1) == 0);
    CHECK(call6(SYS_NET_TCP_SEND, 1, 0x1000, 4, 1000, 0, 0, 1) == ((uint64_t)(-14)));
    CHECK(call6(SYS_NET_TCP_RECV, 1, 0x47000000, 16, 1000, 0, 0, 1) == ((uint64_t)(-14)));
    CHECK(call(SYS_NET_TCP_CLOSE, 1) == 0);
    CHECK(call(SYS_GETPID, 0, 0, 1) == 5 && call(SYS_GETPID, 0, 0, 0) == 1);
    CHECK(call(99) == ERR(38));

    /* write(buf,len): EL0 pointers are validated, kernel pointers are not */
    char kbuf[8] = "abc";
    CHECK(call(SYS_WRITE_BUF, 0x1000, 4) == ERR(14));          /* kernel VA from EL0 */
    CHECK(call(SYS_WRITE_BUF, 0, 4) == ERR(14));
    CHECK(call(SYS_WRITE_BUF, 0x47000FFE, 8) == ERR(14));      /* crosses window end */
    CHECK(call(SYS_WRITE_BUF, 0x47000000, SYSCALL_IO_MAX + 1) == ERR(22));
    CHECK(call(SYS_WRITE_BUF, 0x47000000, 0) == 0);
    out_len = 0;
    CHECK(call(SYS_WRITE_BUF, (uint64_t)kbuf, 3, 0) == 3 && out_len == 3 && memcmp(out, "abc", 3) == 0);

    /* uname must demand WRITE permission on the destination */
    CHECK(call(SYS_UNAME, 0x47000000) == ERR(14));             /* read-only page */
    CHECK(call(SYS_UNAME, 0x47000FF0) == ERR(14));
    CHECK(call(SYS_UNAME, 0) == ERR(14));
    char kname[40];
    CHECK(call(SYS_UNAME, (uint64_t)kname, 0, 0) > 0 && strncmp(kname, "MyKernel", 8) == 0);

    /* read: needs write permission, non-blocking, bounded */
    char rbuf[16];
    rx = "hi"; rx_pos = 0;
    CHECK(call(SYS_READ, 0x47000000, 4) == ERR(14));           /* read-only dest */
    CHECK(call(SYS_READ, (uint64_t)rbuf, 16, 0) == 2 && memcmp(rbuf, "hi", 2) == 0);
    CHECK(call(SYS_READ, (uint64_t)rbuf, 16, 0) == 0);         /* nothing pending */
    CHECK(call(SYS_READ, (uint64_t)rbuf, SYSCALL_IO_MAX + 1, 0) == ERR(22));

    CHECK(call(SYS_SLEEP_MS, 50) == 0 && slept == 50);
    CHECK(call(SYS_SLEEP_MS, SYSCALL_SLEEP_MAX_MS + 1) == ERR(22) && slept == 50);
    CHECK(call(SYS_TIME_MS) == 7123);

    /* file syscalls (kernel-side pointers, from_user = 0) */
    char kpath[] = "/storage/home/hello";
    char kdata[16] = "hello nano";
    char kback[32];
    CHECK(call(SYS_FILE_READ, (uint64_t)kpath, (uint64_t)kback, 0, 32) == ERR(2));   /* not created yet */
    CHECK(call(SYS_FILE_WRITE, (uint64_t)kpath, (uint64_t)kdata, 0, 10) == 10);
    CHECK(call(SYS_FILE_READ, (uint64_t)kpath, (uint64_t)kback, 0, 32) == 10 && memcmp(kback, "hello nano", 10) == 0);
    CHECK(call(SYS_FILE_READ, (uint64_t)kpath, (uint64_t)kback, 0, 4) == ERR(27));   /* too big for buffer: EFBIG, not ENOENT */
    CHECK(call(SYS_FILE_READ, (uint64_t)kpath, (uint64_t)kback, 0, 0) == ERR(22));
    CHECK(call(SYS_FILE_READ, (uint64_t)kpath, (uint64_t)kback, 0, SYSCALL_FILE_MAX + 1) == ERR(22));
    CHECK(call(SYS_FILE_WRITE, (uint64_t)kpath, 0, 0, SYSCALL_FILE_MAX + 1) == ERR(22));
    CHECK(call(SYS_FILE_WRITE, (uint64_t)kpath, 0, 0, 0) == 0);                       /* empty file is valid */
    CHECK(call(SYS_FILE_READ, (uint64_t)kpath, (uint64_t)kback, 0, 32) == 0);
    CHECK(call(SYS_FILE_WRITE, (uint64_t)kpath, 0, 0, 5) == ERR(14));                 /* NULL data */
    CHECK(call(SYS_FILE_READ, 0, (uint64_t)kback, 0, 8) == ERR(14));                  /* NULL path */
    /* EL0 pointer validation: kernel VA, read-only dest, unterminated path */
    CHECK(call(SYS_FILE_READ, 0x1000, 0x47001000, 1, 8) == ERR(14));
    CHECK(call(SYS_FILE_WRITE, 0x1000, 0x47001000, 1, 8) == ERR(14));

    /* file syscalls resolve relative to the shell's current directory */
    CHECK(call(SYS_FILE_WRITE, (uint64_t)kpath, (uint64_t)kdata, 0, 3) == 3 && seen_cwd == 7);
    fake_cwd = 9;
    CHECK(call(SYS_FILE_READ, (uint64_t)kpath, (uint64_t)kback, 0, 32) == 3 && seen_cwd == 9);

    /* P2 filesystem metadata and permission-manipulation syscalls. */
    FsStat stat_result;
    CHECK(call(SYS_FS_STAT, (uint64_t)kpath, (uint64_t)&stat_result, 0) == 0);
    CHECK(stat_result.inode == 1 && stat_result.type == FS_FILE && stat_result.size == 3);
    CHECK(stat_result.mode == 0644U && stat_result.uid == 1000U && stat_result.gid == 1000U);
    CHECK(call(SYS_FS_STAT, 0x1000, 0x1000, 1) == ERR(14));
    CHECK(fake_uid == 0 && fake_gid == 0); /* EL0 syscalls keep the launching shell's credentials (root stays root) */
    fs_set_credentials(0U, 0U);
    (void)call(SYS_PING, 0, 0, 1);
    CHECK(fake_uid == 0 && fake_gid == 0);   /* root shell: EL0 stays root */
    fs_set_credentials(1000U, 1000U);
    (void)call(SYS_PING, 0, 0, 1);
    CHECK(fake_uid == 1000U && fake_gid == 1000U); /* user shell: EL0 stays user */
    fs_set_credentials(0U, 0U);
    CHECK(call(SYS_FS_TRUNCATE, (uint64_t)kpath, 8, 0) == 0 && fake_size == 8);
    CHECK(fake_file[3] == 0 && fake_file[7] == 0);
    CHECK(call(SYS_FS_TRUNCATE, (uint64_t)kpath, 1048577ULL, 0) == ERR(27));
    CHECK(call(SYS_FS_CHMOD, (uint64_t)kpath, 0600U, 0) == 0 && fake_mode == 0600U);
    CHECK(call(SYS_FS_CHMOD, (uint64_t)kpath, 01000U, 0) == ERR(22));

    /* getarg: copies the exec argument, truncates to cap-1, validates dest */
    char karg[16];
    CHECK(call(SYS_GETARG, (uint64_t)karg, 16, 0) == 8 && strcmp(karg, "test.txt") == 0);
    CHECK(call(SYS_GETARG, (uint64_t)karg, 4, 0) == 3 && strcmp(karg, "tes") == 0);
    CHECK(call(SYS_GETARG, (uint64_t)karg, 0, 0) == ERR(22));
    CHECK(call(SYS_GETARG, 0, 16, 0) == ERR(14));
    CHECK(call(SYS_GETARG, 0x1000, 16, 1) == ERR(14));

    /* POSIX-like file descriptor syscalls operate on the same VFS backend. */
    fd_init();
    int descriptor = (int)(int64_t)call(SYS_FD_OPEN, (uint64_t)kpath,
        FD_O_CREAT | FD_O_RDWR | FD_O_TRUNC, 0);
    CHECK(descriptor >= 3);
    CHECK(call(SYS_FD_WRITE, (uint64_t)descriptor, (uint64_t)kdata, 0, 5) == 5);
    CHECK(call(SYS_FD_SEEK, (uint64_t)descriptor, 0, 0, 0) == 0);
    memset(kback, 0, sizeof(kback));
    CHECK(call(SYS_FD_READ, (uint64_t)descriptor, (uint64_t)kback, 0, 5) == 5);
    CHECK(memcmp(kback, kdata, 5) == 0);

    /* dup/dup2 share one open-file description, including its seek cursor. */
    int duplicated = (int)(int64_t)call(SYS_FD_DUP, (uint64_t)descriptor, 0, 0);
    CHECK(duplicated >= 3 && duplicated != descriptor);
    CHECK(call(SYS_FD_SEEK, (uint64_t)descriptor, 0, 0, 0) == 0);
    memset(kback, 0, sizeof(kback));
    CHECK(call(SYS_FD_READ, (uint64_t)duplicated, (uint64_t)kback, 0, 2) == 2);
    CHECK(memcmp(kback, "he", 2) == 0);
    CHECK(call(SYS_FD_READ, (uint64_t)descriptor, (uint64_t)kback, 0, 2) == 2);
    CHECK(memcmp(kback, "ll", 2) == 0);
    CHECK(call(SYS_FD_DUP2, (uint64_t)duplicated, 7, 0) == 7);
    CHECK(call(SYS_FD_CLOSE, (uint64_t)duplicated, 0, 0) == 0);
    CHECK(call(SYS_FD_CLOSE, (uint64_t)descriptor, 0, 0) == 0);
    CHECK(call(SYS_FD_READ, (uint64_t)descriptor, (uint64_t)kback, 0, 1) == ERR(9));
    CHECK(call(SYS_FD_CLOSE, 7, 0, 0) == 0);
    CHECK(call(SYS_FD_DUP, 99, 0, 0) == ERR(9));
    CHECK(call(SYS_FD_DUP2, 0, 99, 0) == ERR(9));

    /* The generic non-blocking kernel pipe is available through the syscall ABI. */
    int pipefd[2] = {-1, -1};
    CHECK(call(SYS_FD_PIPE, (uint64_t)pipefd, 0, 0) == 0);
    CHECK(pipefd[0] >= 3 && pipefd[1] >= 3 && pipefd[0] != pipefd[1]);
    CHECK(call(SYS_FD_READ, (uint64_t)pipefd[0], (uint64_t)kback, 0, 1) == ERR(11));
    CHECK(call(SYS_FD_WRITE, (uint64_t)pipefd[1], (uint64_t)kdata, 0, 5) == 5);
    memset(kback, 0, sizeof(kback));
    CHECK(call(SYS_FD_READ, (uint64_t)pipefd[0], (uint64_t)kback, 0, 5) == 5);
    CHECK(memcmp(kback, kdata, 5) == 0);
    CHECK(call(SYS_FD_CLOSE, (uint64_t)pipefd[1], 0, 0) == 0);
    CHECK(call(SYS_FD_READ, (uint64_t)pipefd[0], (uint64_t)kback, 0, 1) == 0);
    CHECK(call(SYS_FD_CLOSE, (uint64_t)pipefd[0], 0, 0) == 0);
    CHECK(call(SYS_FD_PIPE, 0x1000, 0, 1) == ERR(14));

    CHECK(call(SYS_FD_OPEN, 0x1000, FD_O_RDONLY, 1) == ERR(14));
    CHECK(call(SYS_FD_OPEN, (uint64_t)kpath, 0x100000000ULL, 0) == ERR(22));
    CHECK(call(SYS_FD_READ, (uint64_t)descriptor, (uint64_t)kback, 0,
               FD_IO_MAX + 1) == ERR(22));

    /* P3 UDP/DNS syscalls: owner-scoped handles and bounded EL0 pointers. */
    CHECK(call(SYS_NET_UDP_SOCKET, 0, 0, 1) == 1);
    CHECK(call(SYS_NET_UDP_BIND, 1, 5300, 1) == 0);
    CHECK(call(SYS_NET_UDP_BIND, 1, 65536, 1) == ERR(22));
    CHECK(call6(SYS_NET_UDP_SENDTO, 1, 0x0A000203U, 53, 0x1000, 4, 0, 1) == ERR(14));
    CHECK(call6(SYS_NET_UDP_SENDTO, 1, 0x0A000203U, 53, (uint64_t)kdata, 4, 0, 0) == 4);
    CHECK(call6(SYS_NET_UDP_RECVFROM, 1, 0x1000, (uint64_t)kback, (uint64_t)kback, 8, 100, 1) == ERR(14));
    CHECK(call6(SYS_NET_UDP_RECVFROM, 1, (uint64_t)&stat_result, (uint64_t)&fake_mode,
                (uint64_t)kback, 8, NET_UDP_MAX_TIMEOUT_MS + 1, 0) == ERR(22));
    CHECK(call(SYS_NET_UDP_CLOSE, 1, 0, 1) == 0);
    uint32_t dns_ip = 0;
    char dns_name[] = "example.com";
    CHECK(call6(SYS_NET_DNS_LOOKUP, (uint64_t)dns_name, (uint64_t)&dns_ip,
                500, 0, 0, 0, 0) == 0 && dns_ip == 0x5DB8D822U);
    CHECK(call6(SYS_NET_DNS_LOOKUP, 0x1000, 0x1000, 500, 0, 0, 0, 1) == ERR(14));
    CHECK(call6(SYS_NET_DNS_LOOKUP, (uint64_t)dns_name, (uint64_t)&dns_ip,
                NET_UDP_MAX_TIMEOUT_MS + 1ULL, 0, 0, 0, 0) == ERR(22));

    /* SVC immediate != 0 is rejected */
    ExceptionFrame f; memset(&f, 0, sizeof f); f.esr = 5; f.x[8] = SYS_PING;
    syscall_dispatch(&f, 1);
    CHECK(f.x[0] == ERR(22));

    /* exit redirects the frame and records the code */
    memset(&f, 0, sizeof f); f.x[8] = SYS_EXIT; f.x[0] = 3;
    syscall_dispatch(&f, 1);
    CHECK(exit_code == 3 && f.spsr == 0x3C5ULL && f.elr != 0);
    CHECK(call(SYS_EXIT, 0, 0, 0) == ERR(1));                  /* EL1 may not exit */

    printf("SYSCALL HOST TEST: PASS\n");
    return 0;
}
