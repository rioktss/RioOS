#include "fs.h"
#include "types.h"
#include <stdio.h>
#include <stdlib.h>

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
    for(int i=0;i<900;i++)dst[i]=0; got=0;
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
    fclose(g_file); remove(path); puts("FS HOST TEST: PASS"); return 0;
}
