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
    if(fs_rm("/storage/home/test.bin",fs_get_root())!=0)fail("remove");
    fs_init(); if(fs_resolve("/storage/home/test.bin",fs_get_root())>=0)fail("remove persistence");
    fclose(g_file); remove(path); puts("FS HOST TEST: PASS"); return 0;
}
