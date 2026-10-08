#include "fs.h"
#include "vfs.h"
#include "types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FILE* g_file = 0;
static const uint64_t SECTOR = 512;
static const uint64_t SECTORS = 32768;
static char out[8192]; static unsigned out_len = 0;

void uart_putc(char c) { if (out_len < sizeof(out) - 1) { out[out_len++] = c; out[out_len] = 0; } }
void uart_puts(const char* s) { while (*s) uart_putc(*s++); }
void storage_init() {}
int storage_ready() { return g_file != 0; }
int storage_read_sector(uint64_t s, void* b)
{ if(!g_file||!b||s>=SECTORS)return -1; if(fseek(g_file,(long)(s*SECTOR),SEEK_SET))return -1; return fread(b,1,SECTOR,g_file)==SECTOR?0:-1; }
int storage_write_sector(uint64_t s, const void* b)
{ if(!g_file||!b||s>=SECTORS)return -1; if(fseek(g_file,(long)(s*SECTOR),SEEK_SET))return -1; if(fwrite(b,1,SECTOR,g_file)!=SECTOR)return -1; fflush(g_file); return 0; }
int storage_flush() { return g_file && fflush(g_file) == 0 ? 0 : -1; }

#define CHECK(c) do { if (!(c)) { printf("VFS HOST TEST FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (0)

int main()
{
    const char* path = "build/vfs-host-test.img";
    g_file = fopen(path, "w+b"); CHECK(g_file);
    CHECK(fseek(g_file, (long)(SECTOR * SECTORS - 1), SEEK_SET) == 0 && fputc(0, g_file) != EOF);
    fs_init(); vfs_init();

    char cwd[64];
    vfs_get_path(cwd, sizeof cwd);
    CHECK(strcmp(cwd, "/") == 0);

    CHECK(vfs_cd("/storage/home") == 0);
    vfs_get_path(cwd, sizeof cwd);
    CHECK(strcmp(cwd, "/storage/home") == 0);
    vfs_get_path(cwd, 6);                       /* truncates, stays terminated */
    CHECK(strlen(cwd) == 5 && cwd[5] == 0);

    CHECK(vfs_write("note.txt", "hello\nworld") == 0);
    CHECK(vfs_touch("note.txt") == -2);
    CHECK(vfs_touch(".hidden") >= 0);
    out_len = 0; out[0] = 0;
    vfs_ls();
    CHECK(strstr(out, "note.txt") && strstr(out, "(11 bytes)"));
    CHECK(strstr(out, ".hidden") == 0);
    out_len = 0; out[0] = 0;
    vfs_ls("-la");
    CHECK(strstr(out, ".hidden") != 0);
    CHECK(strstr(out, "-rw-r--r--") != 0);
    out_len = 0; out[0] = 0;
    vfs_ls("-l /home");
    CHECK(strstr(out, "drwxr-xr-x") != 0 && strstr(out, "user") != 0);
    out_len = 0; out[0] = 0;
    vfs_ls("/");
    CHECK(strstr(out, "[DIR]  storage") != 0);
    out_len = 0; out[0] = 0;
    vfs_ls("nothing");
    CHECK(strstr(out, "ls: not found") != 0);
    out_len = 0; out[0] = 0;
    vfs_ls("note.txt");
    CHECK(strstr(out, "[FILE] note.txt") != 0);

    out_len = 0; out[0] = 0;
    CHECK(vfs_cat_numbered("note.txt") == 0);
    CHECK(strstr(out, "     1  hello\n     2  world") != 0);

    CHECK(vfs_mkdir_p("/home/user/docs/projects") >= 0);
    CHECK(vfs_cd("/home/user/docs/projects") == 0);
    vfs_get_path(cwd, sizeof cwd);
    CHECK(strcmp(cwd, "/home/user/docs/projects") == 0);
    CHECK(vfs_touch("empty.txt") >= 0);
    out_len = 0; out[0] = 0;
    vfs_ls("-la ..");
    CHECK(strstr(out, "projects") != 0 && strstr(out, "empty.txt") == 0);

    out_len = 0; out[0] = 0;
    vfs_ls("-R /home/user");
    CHECK(strstr(out, "/home/user/docs:") != 0);
    CHECK(strstr(out, "/home/user/docs/projects:") != 0);

    /* recursive delete removes the full tree */
    CHECK(vfs_rm_recursive("/home/user/docs") == 0);
    CHECK(fs_resolve("/home/user/docs", vfs_cwd()) < 0);

    /* rm of the directory we stand in must move the shell to root */
    CHECK(vfs_mkdir("/tmpdir") >= 0 && vfs_cd("/tmpdir") == 0);
    CHECK(vfs_rm(".") == 0);
    CHECK(vfs_cwd() == fs_get_root());
    vfs_get_path(cwd, sizeof cwd);
    CHECK(strcmp(cwd, "/") == 0);

    fclose(g_file); remove(path);
    puts("VFS HOST TEST: PASS");
    return 0;
}
