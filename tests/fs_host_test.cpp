#include "fs.h"
#include "types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FILE* g_file = 0;
static const uint64_t SECTOR = 512;
static const uint64_t SECTORS = 32768;

void uart_putc(char c) { (void)c; }
void uart_puts(const char* s) { (void)s; }
void storage_init() {}
int storage_ready() { return g_file != 0; }
int storage_read_sector(uint64_t sector, void* buffer)
{
    if(!g_file || !buffer || sector>=SECTORS) return -1;
    if(fseek(g_file,(long)(sector*SECTOR),SEEK_SET)!=0) return -1;
    return fread(buffer,1,SECTOR,g_file)==SECTOR?0:-1;
}
int storage_write_sector(uint64_t sector,const void* buffer)
{
    if(!g_file || !buffer || sector>=SECTORS) return -1;
    if(fseek(g_file,(long)(sector*SECTOR),SEEK_SET)!=0) return -1;
    if(fwrite(buffer,1,SECTOR,g_file)!=SECTOR) return -1;
    fflush(g_file); return 0;
}
int storage_flush(){return g_file&&fflush(g_file)==0?0:-1;}

static void fail(const char* m){fprintf(stderr,"FS HOST TEST FAIL: %s\n",m);exit(1);}
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FS HOST TEST FAIL line %d: %s\n", __LINE__, #c); fail(#c); } } while (0)
static int same(const unsigned char*a,const unsigned char*b,unsigned long n){for(unsigned long i=0;i<n;i++)if(a[i]!=b[i])return 0;return 1;}

int main(){
    const char* path="build/fs-host-test.img";
    g_file=fopen(path,"w+b"); if(!g_file)fail("open");
    if(fseek(g_file,(long)(SECTOR*SECTORS-1),SEEK_SET)!=0||fputc(0,g_file)==EOF||fflush(g_file)!=0)fail("size");
    fs_init(); int root=fs_get_root(); if(root<0)fail("root");
    if(fs_resolve("/storage/home",root)<0)fail("home");
    if(fs_touch("/storage/home/test.bin",root)<0)fail("touch");
    unsigned char src[900],dst[900]; for(int i=0;i<900;i++)src[i]=(unsigned char)((i*37U)^0x5A);
    if(fs_write_data("/storage/home/test.bin",root,src,900)!=0)fail("write");
    uint64_t got=0; if(fs_read_file("/storage/home/test.bin",root,dst,900,&got)!=0||got!=900||!same(src,dst,900))fail("read");
    fs_init();
    for(int i=0;i<900;i++)dst[i]=0;
    got=0;
    if(fs_read_file("/storage/home/test.bin",fs_get_root(),dst,900,&got)!=0||got!=900||!same(src,dst,900))fail("remount persistence");
    if(fs_get_mode(fs_resolve("/storage/home/test.bin",fs_get_root())) != 0644U) fail("default file mode");
    {
        int id = fs_resolve("/storage/home/test.bin", fs_get_root());
        if (fs_set_mode(id, 0751U) != 0) fail("chmod mode set");
        fs_init();
        id = fs_resolve("/storage/home/test.bin", fs_get_root());
        if (fs_get_mode(id) != 0751U) fail("chmod mode persistence");
        if (fs_total_sectors() != SECTORS) fail("disk sector total");
        if (fs_used_sectors() == 0) fail("disk sector usage");
    }
    if(fs_rm("/storage/home/test.bin",fs_get_root())!=0)fail("remove");
    if(fs_mkdir_p("/storage/home/a/b/c",fs_get_root())<0)fail("mkdir -p");
    if(fs_touch("/storage/home/a/.hidden",fs_get_root())<0)fail("hidden touch");
    if(fs_rm_recursive("/storage/home/a",fs_get_root())!=0)fail("recursive remove");
    if(fs_resolve("/storage/home/a/b/c",fs_get_root())>=0)fail("recursive remove persistence");
    fs_init(); if(fs_resolve("/storage/home/test.bin",fs_get_root())>=0)fail("remove persistence");
    /* ---- fs_rename (mv) ---- */
    {
        int home=fs_resolve("/storage/home",fs_get_root());
        if(fs_touch("a.txt",home)<0)fail("rename: touch");
        if(fs_write_data("a.txt",home,src,300)!=0)fail("rename: write");
        if(fs_rename("a.txt","b.txt",home)!=0)fail("rename: same dir");
        if(fs_resolve("a.txt",home)>=0||fs_resolve("b.txt",home)<0)fail("rename: result");
        if(fs_mkdir("/mvdir",fs_get_root())<0)fail("rename: mkdir");
        if(fs_rename("b.txt","/mvdir",home)!=0)fail("rename: into dir");
        got=0; if(fs_read_file("/mvdir/b.txt",fs_get_root(),dst,900,&got)!=0||got!=300||!same(src,dst,300))fail("rename: data kept");
        if(fs_mkdir("/mvdir/sub",fs_get_root())<0)fail("rename: mkdir sub");
        if(fs_rename("/mvdir","/mvdir/sub",fs_get_root())!=-4)fail("rename: into itself");
        if(fs_touch("/x.txt",fs_get_root())<0)fail("rename: touch x");
        if(fs_rename("/x.txt","/mvdir/b.txt",fs_get_root())!=-2)fail("rename: exists");
        if(fs_rename("/",  "/zzz",fs_get_root())!=-3)fail("rename: root");
        if(fs_rename("/nope","/zzz",fs_get_root())!=-1)fail("rename: missing");
        if(fs_rename("/x.txt","/nodir/y.txt",fs_get_root())!=-5)fail("rename: bad parent");
        if(fs_rename("/mvdir","/renamed",fs_get_root())!=0)fail("rename: dir");
        fs_init();
        if(fs_resolve("/renamed/b.txt",fs_get_root())<0||fs_resolve("/mvdir",fs_get_root())>=0)fail("rename persistence");
    }
    /* P2: persisted ownership, permission enforcement, stat and truncate. */
    {
        int root = fs_get_root();
        const char* locked = "/p2-root-secret.txt";
        CHECK(fs_touch(locked, root) >= 0);
        CHECK(fs_write_data(locked, root, (const uint8_t*)"secret", 6) == 0);
        int locked_id = fs_resolve(locked, root);
        CHECK(locked_id >= 0 && fs_get_uid(locked_id) == 0U && fs_get_gid(locked_id) == 0U);
        CHECK(fs_set_mode(locked_id, 0600U) == 0);

        fs_set_credentials(1000U, 1000U);
        unsigned char readbuf[32] = {0}; uint64_t readsize = 0;
        CHECK(fs_read_file(locked, root, readbuf, sizeof(readbuf), &readsize) == -13);
        CHECK(fs_write_data(locked, root, (const uint8_t*)"hack", 4) == -13);
        CHECK(fs_set_mode(locked_id, 0644U) == -13);
        CHECK(fs_set_owner(locked_id, 1000U, 1000U) == -13);
        CHECK(fs_mkdir("/p2-user-must-not-create-at-root", root) == -13);

        const char* owned = "/storage/home/p2-owned.txt";
        CHECK(fs_touch(owned, root) >= 0);
        CHECK(fs_write_data(owned, root, (const uint8_t*)"abc", 3) == 0);
        int owned_id = fs_resolve(owned, root);
        CHECK(owned_id >= 0 && fs_get_uid(owned_id) == 1000U && fs_get_gid(owned_id) == 1000U);
        CHECK(fs_set_mode(owned_id, 0600U) == 0); /* owner may chmod own file */
        FsStat st;
        CHECK(fs_stat_path(owned, root, &st) == 0);
        CHECK(st.inode == (uint32_t)owned_id && st.type == FS_FILE && st.mode == 0600U);
        CHECK(st.uid == 1000U && st.gid == 1000U && st.size == 3U);
        CHECK(fs_truncate(owned, root, 5) == 0);
        memset(readbuf, 0xFF, sizeof(readbuf)); readsize = 0;
        CHECK(fs_read_file(owned, root, readbuf, sizeof(readbuf), &readsize) == 0);
        CHECK(readsize == 5 && readbuf[0] == 'a' && readbuf[1] == 'b' && readbuf[2] == 'c' && readbuf[3] == 0 && readbuf[4] == 0);
        CHECK(fs_truncate(owned, root, 1) == 0);
        CHECK(fs_truncate(owned, root, 1048577ULL) == -27);

        /* Search permission must be required before processing dot-dot. */
        fs_set_credentials(1000U, 1000U);
        CHECK(fs_mkdir("/storage/home/p2-no-search", root) >= 0);
        int no_search_id = fs_resolve("/storage/home/p2-no-search", root);
        CHECK(no_search_id >= 0 && fs_set_mode(no_search_id, 0600U) == 0);
        CHECK(fs_resolve("/storage/home/p2-no-search/../p2-owned.txt", root) == -13);
        CHECK(fs_mkdir_p("/storage/home/p2-no-search/../p2-created-escape", root) == -13);
        CHECK(fs_resolve("/storage/home/p2-created-escape", root) == -1);
        fs_set_credentials(0U, 0U);
        CHECK(fs_rm("/storage/home/p2-no-search", root) == 0);

        CHECK(fs_set_owner(owned_id, 2000U, 2000U) == 0);
        CHECK(fs_get_uid(owned_id) == 2000U && fs_get_gid(owned_id) == 2000U);

        /* Legacy v1 inode: clear the P2 metadata marker and verify conservative migration. */
        const char* legacy = "/storage/home/p2-legacy.txt";
        CHECK(fs_touch(legacy, root) >= 0);
        int legacy_id = fs_resolve(legacy, root);
        CHECK(legacy_id >= 0);
        /* Also test a legacy file outside user homes is conservatively system-owned. */
        long marker_offset = (long)(512ULL + (uint64_t)legacy_id * 128ULL + 120ULL);
        long locked_marker_offset = (long)(512ULL + (uint64_t)locked_id * 128ULL + 120ULL);
        CHECK(fseek(g_file, marker_offset, SEEK_SET) == 0);
        const unsigned char zeros[4] = {0,0,0,0};
        CHECK(fwrite(zeros, 1, sizeof(zeros), g_file) == sizeof(zeros));
        CHECK(fseek(g_file, locked_marker_offset, SEEK_SET) == 0);
        CHECK(fwrite(zeros, 1, sizeof(zeros), g_file) == sizeof(zeros));
        CHECK(fflush(g_file) == 0);
        fs_init();
        legacy_id = fs_resolve(legacy, fs_get_root());
        CHECK(legacy_id >= 0 && fs_get_uid(legacy_id) == 1000U && fs_get_gid(legacy_id) == 1000U);
        locked_id = fs_resolve(locked, fs_get_root());
        CHECK(locked_id >= 0 && fs_get_uid(locked_id) == 0U && fs_get_gid(locked_id) == 0U);
        CHECK(fs_get_uid(fs_resolve("/bin", fs_get_root())) == 0U);
        owned_id = fs_resolve(owned, fs_get_root());
        CHECK(owned_id >= 0 && fs_get_uid(owned_id) == 2000U && fs_get_gid(owned_id) == 2000U);
        CHECK(fs_stat_path(owned, fs_get_root(), &st) == 0 && st.uid == 2000U && st.gid == 2000U);
    }

    /* Root editing inside a user's home must not lock the user out of the file. */
    {
        int root = fs_get_root();
        uint8_t data[3] = {1, 2, 3};
        fs_set_credentials(0U, 0U);
        CHECK(fs_write_data("/home/user/rootmade.txt", root, data, 3) == 0);
        int id = fs_resolve("/home/user/rootmade.txt", root);
        CHECK(id >= 0 && fs_get_uid(id) == 1000U && fs_get_gid(id) == 1000U);
        CHECK(fs_set_owner(id, 0U, 0U) == 0);            /* simulate an old root-owned file */
        CHECK(fs_get_uid(id) == 0U);
        fs_init();                                        /* remount repairs it */
        fs_set_credentials(0U, 0U);
        root = fs_get_root();
        id = fs_resolve("/home/user/rootmade.txt", root);
        CHECK(id >= 0 && fs_get_uid(id) == 1000U);
        fs_set_credentials(1000U, 1000U);
        CHECK(fs_write_data("/home/user/rootmade.txt", root, data, 3) == 0);
        fs_set_credentials(0U, 0U);
        CHECK(fs_touch("/rootonly.txt", root) >= 0);      /* outside home stays root-owned */
        CHECK(fs_get_uid(fs_resolve("/rootonly.txt", root)) == 0U);
    }

    /* A corrupted UINT64_MAX file size must be rejected rather than wrapping
       the sector-count calculation to zero and being accepted as an empty file. */
    {
        int root = fs_get_root();
        fs_set_credentials(0U, 0U);
        CHECK(fs_touch("/p2-corrupt-size.bin", root) >= 0);
        int bad_id = fs_resolve("/p2-corrupt-size.bin", root);
        CHECK(bad_id >= 0);
        const long size_offset = (long)(512ULL + (uint64_t)bad_id * 128ULL + 16ULL);
        const unsigned char impossible_size[8] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
        CHECK(fseek(g_file, size_offset, SEEK_SET) == 0);
        CHECK(fwrite(impossible_size, 1, sizeof(impossible_size), g_file) == sizeof(impossible_size));
        CHECK(fflush(g_file) == 0);
        fs_init();
        CHECK(fs_persistent() == 0);
        CHECK(fs_resolve("/p2-corrupt-size.bin", fs_get_root()) < 0);
    }

    fclose(g_file); remove(path); puts("FS HOST TEST: PASS"); return 0;
}
