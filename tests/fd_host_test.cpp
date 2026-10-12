#include "fd.h"
#include "fs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FILE* disk = 0;
static const uint64_t SECTOR_SIZE = 512;
static const uint64_t SECTOR_COUNT = 32768;
static char uart_output[256];
static unsigned uart_output_len = 0;
static const char* uart_input = "";
static unsigned uart_input_pos = 0;

void uart_putc(char c)
{
    if (uart_output_len + 1 < sizeof(uart_output))
    {
        uart_output[uart_output_len++] = c;
        uart_output[uart_output_len] = '\0';
    }
}
void uart_puts(const char* s) { if (s) while (*s) uart_putc(*s++); }
int uart_try_getc()
{
    return uart_input[uart_input_pos]
        ? (unsigned char)uart_input[uart_input_pos++] : -1;
}
int uart_has_data() { return uart_input[uart_input_pos] != '\0'; }
char uart_getc() { int c = uart_try_getc(); return c < 0 ? '\0' : (char)c; }

void storage_init() {}
int storage_ready() { return disk != 0; }
int storage_read_sector(uint64_t sector, void* buffer)
{
    if (!disk || !buffer || sector >= SECTOR_COUNT) return -1;
    if (fseek(disk, (long)(sector * SECTOR_SIZE), SEEK_SET) != 0) return -1;
    return fread(buffer, 1, SECTOR_SIZE, disk) == SECTOR_SIZE ? 0 : -1;
}
int storage_write_sector(uint64_t sector, const void* buffer)
{
    if (!disk || !buffer || sector >= SECTOR_COUNT) return -1;
    if (fseek(disk, (long)(sector * SECTOR_SIZE), SEEK_SET) != 0) return -1;
    if (fwrite(buffer, 1, SECTOR_SIZE, disk) != SECTOR_SIZE) return -1;
    return fflush(disk) == 0 ? 0 : -1;
}
int storage_flush() { return disk && fflush(disk) == 0 ? 0 : -1; }

#define CHECK(c) do { if (!(c)) { printf("FD HOST TEST FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (0)

int main()
{
    const char* disk_path = "build/fd-host-test.img";
    disk = fopen(disk_path, "w+b"); CHECK(disk);
    CHECK(fseek(disk, (long)(SECTOR_SIZE * SECTOR_COUNT - 1), SEEK_SET) == 0);
    CHECK(fputc(0, disk) != EOF && fflush(disk) == 0);

    fs_init();
    int root = fs_get_root();
    CHECK(root >= 0 && fs_resolve("/storage/home", root) >= 0);
    fd_init();

    const char* path = "/storage/home/fd-host.txt";
    int fd = fd_open(path, FD_O_CREAT | FD_O_RDWR | FD_O_TRUNC, root);
    CHECK(fd >= 3 && fd < FD_MAX_DESCRIPTORS);
    CHECK(fd_write(fd, "hello", 5) == 5);
    CHECK(fd_seek(fd, 0, 0) == 0);
    char buffer[32]; memset(buffer, 0, sizeof(buffer));
    CHECK(fd_read(fd, buffer, 5) == 5 && memcmp(buffer, "hello", 5) == 0);
    CHECK(fd_seek(fd, 0, 2) == 5);
    CHECK(fd_write(fd, "!", 1) == 1);
    CHECK(fd_seek(fd, -2, 2) == 4);
    memset(buffer, 0, sizeof(buffer));
    CHECK(fd_read(fd, buffer, 2) == 2 && memcmp(buffer, "o!", 2) == 0);
    CHECK(fd_close(fd) == 0);
    CHECK(fd_read(fd, buffer, 1) == -9);

    int append = fd_open(path, FD_O_WRONLY | FD_O_APPEND, root);
    CHECK(append >= 3);
    CHECK(fd_write(append, "+", 1) == 1);
    CHECK(fd_close(append) == 0);

    int readonly = fd_open(path, FD_O_RDONLY, root);
    CHECK(readonly >= 3);
    CHECK(fd_write(readonly, "x", 1) == -9);
    CHECK(fd_seek(readonly, -1, 0) == -22);
    CHECK(fd_close(readonly) == 0);

    uint8_t actual[32]; uint64_t size = 0;
    memset(actual, 0, sizeof(actual));
    CHECK(fs_read_file(path, root, actual, sizeof(actual), &size) == 0);
    CHECK(size == 7 && memcmp(actual, "hello!+", 7) == 0);

    int stale = fd_open(path, FD_O_RDWR, root);
    CHECK(stale >= 3 && fs_rm(path, root) == 0);
    CHECK(fd_write(stale, "z", 1) == -2);
    CHECK(fd_read(stale, buffer, 1) == -2);
    CHECK(fd_close(stale) == 0);

    CHECK(fd_open("/storage/home/no-such-file", FD_O_RDONLY, root) == -2);
    CHECK(fd_open("/storage/home/no-such-file", FD_O_WRONLY | FD_O_CREAT, root) >= 3);
    fd_close_all();
    uart_output_len = 0; uart_output[0] = '\0';
    CHECK(fd_write(1, "ok", 2) == 2);
    CHECK(strcmp(uart_output, "ok") == 0);
    uart_input = "hi"; uart_input_pos = 0;
    memset(buffer, 0, sizeof(buffer));
    CHECK(fd_read(0, buffer, 2) == 2 && memcmp(buffer, "hi", 2) == 0);
    CHECK(fd_read(0, 0, 0) == 0 && fd_write(1, 0, 0) == 0);
    CHECK(fd_seek(1, 0, 0) == -29);
    CHECK(fd_open("/storage/home", FD_O_RDONLY, root) == -21);
    CHECK(fd_open(path, FD_O_RDWR | 0x800U, root) == -22);

    /* Recreate the earlier test file, then check dup's shared open-file cursor. */
    CHECK(fs_write_data(path, root, (const uint8_t*)"hello!+", 7) == 0);
    /* dup must share the same open-file cursor, as Unix-like descriptors do. */
    int shared = fd_open(path, FD_O_RDONLY, root); CHECK(shared >= 3);
    int shared_dup = fd_dup(shared); CHECK(shared_dup >= 3 && shared_dup != shared);
    CHECK(fd_read(shared_dup, buffer, 2) == 2 && memcmp(buffer, "he", 2) == 0);
    CHECK(fd_read(shared, buffer, 2) == 2 && memcmp(buffer, "ll", 2) == 0);
    CHECK(fd_dup2(shared, 7) == 7);
    CHECK(fd_close(shared_dup) == 0);
    CHECK(fd_close(shared) == 0);
    CHECK(fd_close(7) == 0);
    CHECK(fd_dup(99) == -9 && fd_dup2(99, 4) == -9 && fd_dup2(0, 99) == -9);

    /* Descriptor tables are isolated by owner PID, while each table gets 0/1/2. */
    int owner0_fd = fd_open(path, FD_O_RDONLY, root); CHECK(owner0_fd >= 3);
    CHECK(fd_set_process(77) == 0);
    CHECK(fd_read(owner0_fd, buffer, 1) == -9);
    int owner77_fd = fd_open(path, FD_O_RDONLY, root); CHECK(owner77_fd == 3);
    CHECK(fd_read(owner77_fd, buffer, 2) == 2 && memcmp(buffer, "he", 2) == 0);
    fd_release_process(77);
    CHECK(fd_set_process(0) == 0);
    CHECK(fd_read(owner0_fd, buffer, 2) == 2 && memcmp(buffer, "he", 2) == 0);
    CHECK(fd_close(owner0_fd) == 0);

    /* Generic kernel pipe: partial/non-blocking semantics, EOF and EPIPE. */
    int pfd[2] = {-1, -1};
    CHECK(fd_pipe(pfd) == 0 && pfd[0] >= 3 && pfd[1] >= 3 && pfd[0] != pfd[1]);
    CHECK(fd_read(pfd[0], buffer, 1) == -11); /* empty but writer alive */
    int pdup = fd_dup(pfd[1]); CHECK(pdup >= 3);
    CHECK(fd_close(pfd[1]) == 0);
    CHECK(fd_write(pdup, "pipe", 4) == 4);
    memset(buffer, 0, sizeof(buffer));
    CHECK(fd_read(pfd[0], buffer, 4) == 4 && memcmp(buffer, "pipe", 4) == 0);
    CHECK(fd_close(pdup) == 0);
    CHECK(fd_read(pfd[0], buffer, 1) == 0); /* no writers: EOF */
    CHECK(fd_close(pfd[0]) == 0);

    int pfd2[2] = {-1, -1};
    CHECK(fd_pipe(pfd2) == 0);
    CHECK(fd_close(pfd2[0]) == 0);
    CHECK(fd_write(pfd2[1], "x", 1) == -32); /* EPIPE/no readers */
    CHECK(fd_close(pfd2[1]) == 0);

    /* Fill, drain, wrap, and refill the bounded pipe ring. */
    int ringfd[2] = {-1, -1};
    CHECK(fd_pipe(ringfd) == 0);
    uint8_t ring_in[FD_PIPE_CAPACITY];
    uint8_t ring_out[FD_PIPE_CAPACITY];
    uint8_t refill[FD_PIPE_CAPACITY];
    for (int i = 0; i < FD_PIPE_CAPACITY; ++i)
    {
        ring_in[i] = (uint8_t)(i % 251);
        refill[i] = (uint8_t)(255 - (i % 251));
        ring_out[i] = 0;
    }
    CHECK(fd_write(ringfd[1], ring_in, FD_PIPE_CAPACITY) == FD_PIPE_CAPACITY);
    CHECK(fd_write(ringfd[1], "x", 1) == -11); /* full ring */
    CHECK(fd_read(ringfd[0], ring_out, 3072) == 3072);
    CHECK(memcmp(ring_out, ring_in, 3072) == 0);
    CHECK(fd_write(ringfd[1], refill, 3072) == 3072); /* wraps at end of ring */
    CHECK(fd_read(ringfd[0], ring_out, FD_PIPE_CAPACITY) == FD_PIPE_CAPACITY);
    CHECK(memcmp(ring_out, ring_in + 3072, 1024) == 0);
    CHECK(memcmp(ring_out + 1024, refill, 3072) == 0);
    CHECK(fd_read(ringfd[0], ring_out, 1) == -11);
    CHECK(fd_close(ringfd[1]) == 0);
    CHECK(fd_read(ringfd[0], ring_out, 1) == 0);
    CHECK(fd_close(ringfd[0]) == 0);

    /* dup2 closes its target and binds a new slot to the same open description. */
    int pfd3[2] = {-1, -1};
    CHECK(fd_pipe(pfd3) == 0);
    CHECK(fd_dup2(pfd3[1], 8) == 8);
    CHECK(fd_close(pfd3[1]) == 0);
    CHECK(fd_write(8, "z", 1) == 1);
    CHECK(fd_read(pfd3[0], buffer, 1) == 1 && buffer[0] == 'z');
    CHECK(fd_close(8) == 0 && fd_close(pfd3[0]) == 0);

    /* A write-only file remains writable without granting read permission. */
    fs_set_credentials(1000U, 1000U);
    const char* write_only_path = "/storage/home/write-only.txt";
    CHECK(fs_touch(write_only_path, root) >= 0);
    CHECK(fs_write_data(write_only_path, root, (const uint8_t*)"abc", 3) == 0);
    int write_only_id = fs_resolve(write_only_path, root);
    CHECK(write_only_id >= 0 && fs_set_mode(write_only_id, 0200U) == 0);
    int write_only_fd = fd_open(write_only_path, FD_O_WRONLY, root);
    CHECK(write_only_fd >= 3);
    CHECK(fd_seek(write_only_fd, 0, 0) == 0);
    CHECK(fd_write(write_only_fd, "Q", 1) == 1);
    CHECK(fd_read(write_only_fd, buffer, 1) == -9);
    CHECK(fs_read_file(write_only_path, root, actual, sizeof(actual), &size) == -13);
    CHECK(fd_close(write_only_fd) == 0);
    fs_set_credentials(0U, 0U);
    memset(actual, 0, sizeof(actual)); size = 0;
    CHECK(fs_read_file(write_only_path, root, actual, sizeof(actual), &size) == 0);
    CHECK(size == 3 && memcmp(actual, "Qbc", 3) == 0);
    CHECK(fs_rm(write_only_path, root) == 0);

    fclose(disk); disk = 0; remove(disk_path);
    puts("FD HOST TEST: PASS");
    return 0;
}
