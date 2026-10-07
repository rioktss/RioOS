#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <sys/stat.h>

static constexpr uint32_t MAGIC = 0x4D4B4653U;
static constexpr uint32_t VERSION = 1U;
static constexpr uint64_t SECTOR = 512ULL;
static constexpr uint32_t INODES = 64U;
static constexpr uint32_t INODE_SIZE = 128U;
static constexpr uint32_t INODE_SECTORS = 16U;
static constexpr uint32_t BITMAP_SECTORS = 8U;
static constexpr uint32_t INODE_START = 1U;
static constexpr uint32_t BITMAP_START = 17U;
static constexpr uint32_t DATA_START = 25U;
static constexpr uint32_t TOTAL_SECTORS = 32768U;
static constexpr uint32_t NAME_SIZE = 64U;
static constexpr uint32_t MAX_FILE_SECTORS = 512U;

#pragma pack(push,1)
struct Super {
    uint32_t magic, version, sector_size, inode_size, inode_count;
    uint32_t inode_start, inode_sectors, bitmap_start, bitmap_sectors, data_start;
    uint64_t total_sectors;
    uint8_t reserved[512-48];
};
struct Inode {
    uint32_t used, type;
    int32_t parent;
    uint32_t flags;
    uint64_t size, first_sector;
    uint32_t sector_count;
    char name[NAME_SIZE];
    uint8_t reserved[28];
};
#pragma pack(pop)
static_assert(sizeof(Super)==512, "Super size");
static_assert(sizeof(Inode)==128, "Inode size");

static bool read_at(FILE* f,uint64_t off,void* p,size_t n){return std::fseek(f,(long)off,SEEK_SET)==0 && std::fread(p,1,n,f)==n;}
static bool write_at(FILE* f,uint64_t off,const void* p,size_t n){return std::fseek(f,(long)off,SEEK_SET)==0 && std::fwrite(p,1,n,f)==n;}
static bool read_sector(FILE* f,uint64_t s,uint8_t* b){return read_at(f,s*SECTOR,b,SECTOR);}
static bool write_sector(FILE* f,uint64_t s,const uint8_t* b){return write_at(f,s*SECTOR,b,SECTOR);}
static void bit_set(uint8_t* bm,uint32_t s,bool on){uint8_t m=(uint8_t)(1u<<(s&7));if(on)bm[s>>3]|=m;else bm[s>>3]&=(uint8_t)~m;}
static bool bit_get(const uint8_t* bm,uint32_t s){return ((bm[s>>3]>>(s&7))&1u)!=0;}
static int find_inode(const Inode* ins,const char* name,int parent){for(int i=0;i<(int)INODES;i++)if(ins[i].used&&ins[i].parent==parent&&std::strncmp(ins[i].name,name,NAME_SIZE)==0)return i;return -1;}
static int alloc_inode(const Inode* ins){for(int i=0;i<(int)INODES;i++)if(!ins[i].used)return i;return -1;}
static int alloc_run(uint8_t* bm,uint32_t need){if(!need)return 0;uint32_t run=0;for(uint32_t s=DATA_START;s<TOTAL_SECTORS;s++){if(!bit_get(bm,s))++run;else run=0;if(run>=need){uint32_t first=s+1-need;for(uint32_t k=0;k<need;k++)bit_set(bm,first+k,true);return (int)first;}}return -1;}
static bool save_inode_table(FILE* f,const Inode* ins){for(uint32_t s=0;s<INODE_SECTORS;s++){uint8_t sec[512]{};std::memcpy(sec,&ins[s*4],sizeof(Inode)*4);if(!write_sector(f,INODE_START+s,sec))return false;}return true;}
static bool load_inode_table(FILE* f,Inode* ins){for(uint32_t s=0;s<INODE_SECTORS;s++){uint8_t sec[512];if(!read_sector(f,INODE_START+s,sec))return false;std::memcpy(&ins[s*4],sec,sizeof(Inode)*4);}return true;}
static bool save_bitmap(FILE* f,const uint8_t* bm){for(uint32_t s=0;s<BITMAP_SECTORS;s++)if(!write_sector(f,BITMAP_START+s,bm+s*512))return false;return true;}
static bool load_bitmap(FILE* f,uint8_t* bm){for(uint32_t s=0;s<BITMAP_SECTORS;s++)if(!read_sector(f,BITMAP_START+s,bm+s*512))return false;return true;}
static bool save_super(FILE* f){Super sb{};sb.magic=MAGIC;sb.version=VERSION;sb.sector_size=SECTOR;sb.inode_size=INODE_SIZE;sb.inode_count=INODES;sb.inode_start=INODE_START;sb.inode_sectors=INODE_SECTORS;sb.bitmap_start=BITMAP_START;sb.bitmap_sectors=BITMAP_SECTORS;sb.data_start=DATA_START;sb.total_sectors=TOTAL_SECTORS;return write_sector(f,0,(const uint8_t*)&sb);}
static bool read_super(FILE* f,Super& sb){return read_sector(f,0,(uint8_t*)&sb);}
static bool valid_super(const Super& sb){return sb.magic==MAGIC&&sb.version==VERSION&&sb.sector_size==SECTOR&&sb.inode_size==INODE_SIZE&&sb.inode_count==INODES&&sb.data_start==DATA_START&&sb.total_sectors==TOTAL_SECTORS;}
static bool write_host_file(FILE* f, uint64_t first, uint32_t sectors, const std::vector<uint8_t>& data){
    uint8_t sec[512];
    for(uint32_t s=0;s<sectors;s++){
        std::memset(sec,0,sizeof(sec));
        size_t off=(size_t)s*512;
        size_t n=data.size()>off?data.size()-off:0;
        if(n>512)n=512;
        if(n)std::memcpy(sec,data.data()+off,n);
        if(!write_sector(f,first+s,sec))return false;
    }
    return true;
}
static bool fill_file(FILE* f,Inode* ins,uint8_t* bm,int parent,const char* name,const char* host,bool replace){
    FILE* in=std::fopen(host,"rb");if(!in){std::perror(host);return false;}
    std::fseek(in,0,SEEK_END);long signed_size=std::ftell(in);std::fseek(in,0,SEEK_SET);
    if(signed_size<0||(uint64_t)signed_size>512ULL*MAX_FILE_SECTORS){std::fclose(in);return false;}
    size_t size=(size_t)signed_size;std::vector<uint8_t> data(size);
    if(size&&std::fread(data.data(),1,size,in)!=size){std::fclose(in);return false;}std::fclose(in);
    int id=find_inode(ins,name,parent); if(id<0) id=alloc_inode(ins);
    if(id<0)return false; if(ins[id].used&&!replace)return true;
    uint32_t need=(uint32_t)((size+SECTOR-1)/SECTOR);
    uint64_t old_first=ins[id].first_sector; uint32_t old_count=ins[id].sector_count;
    if(ins[id].used && old_count>=need && (!need || old_first>=DATA_START)){
        if(!write_host_file(f,old_first,need,data))return false;
        for(uint32_t s=need;s<old_count;s++) if(old_first+s<TOTAL_SECTORS) bit_set(bm,(uint32_t)old_first+s,false);
        Inode n=ins[id]; n.used=1;n.type=0;n.parent=parent;n.size=size;n.first_sector=need?old_first:0;n.sector_count=need;ins[id]=n;
        return true;
    }
    int first=alloc_run(bm,need);if(need&&first<0)return false;
    if(need&&!write_host_file(f,(uint64_t)first,need,data))return false;
    if(ins[id].used&&old_count)for(uint32_t s=0;s<old_count;s++)if(old_first+s<TOTAL_SECTORS)bit_set(bm,(uint32_t)old_first+s,false);
    Inode n{};n.used=1;n.type=0;n.parent=parent;n.size=size;n.first_sector=need?(uint64_t)first:0;n.sector_count=need;std::strncpy(n.name,name,NAME_SIZE-1);ins[id]=n;return true;
}
static bool ensure_dir(Inode* ins,int parent,const char* name){int id=find_inode(ins,name,parent);if(id>=0)return ins[id].type==1;id=alloc_inode(ins);if(id<0)return false;Inode n{};n.used=1;n.type=1;n.parent=parent;std::strncpy(n.name,name,NAME_SIZE-1);ins[id]=n;return true;}
static const char* basename_of(const char* path){const char* p=strrchr(path,'/'); return p? p+1 : path;}

int main(int argc,char**argv){
    if(argc<3){std::fprintf(stderr,"usage: mkfs_myfs <disk.img> <elf1> [elf2...]\n");return 2;}
    const char* img=argv[1];
    FILE* f=std::fopen(img,"rb+");
    if(!f)f=std::fopen(img,"wb+");
    if(!f){std::perror(img);return 1;}
    if (std::fseek(f, (long)(TOTAL_SECTORS * SECTOR - 1ULL), SEEK_SET)!= 0 ||
        std::fputc(0, f) == EOF) { std::fclose(f); return 1; }
    std::fflush(f);
    Super sb{}; bool valid=read_super(f,sb)&&valid_super(sb);
    Inode ins[INODES]{}; uint8_t bm[4096]{};
    if(valid){
        if(!load_inode_table(f,ins)||!load_bitmap(f,bm)){std::fprintf(stderr,"cannot read existing filesystem\n");std::fclose(f);return 1;}
    }else{
        if(save_super(f)==false){std::fprintf(stderr,"cannot write superblock\n");std::fclose(f);return 1;}
        std::memset(ins,0,sizeof(ins));std::memset(bm,0,sizeof(bm));for(uint32_t s=0;s<DATA_START;s++)bit_set(bm,s,true);
        ins[0].used=1;ins[0].type=1;ins[0].parent=-1;std::strcpy(ins[0].name,"/");
    }
    int root=0;
    if(!ins[root].used||ins[root].type!=1){std::fprintf(stderr,"invalid root inode\n");std::fclose(f);return 1;}
    if(!ensure_dir(ins,root,"storage")||!ensure_dir(ins,root,"bin")){std::fprintf(stderr,"cannot create directories\n");std::fclose(f);return 1;}
    int storage=find_inode(ins,"storage",root);
    if(!ensure_dir(ins,storage,"home")){std::fprintf(stderr,"cannot create home directory\n");std::fclose(f);return 1;}
    int bin=find_inode(ins,"bin",root);

    for(int i=2;i<argc;i++){
        const char* host = argv[i];
        const char* base = basename_of(host);
        const char* guest = base;
        if(std::strcmp(base,"user_init.elf")==0) guest="init.elf";
        // biar build/nano.elf tetap jadi nano.elf
        std::printf("Installing %s -> /bin/%s\n", host, guest);
        if(!fill_file(f,ins,bm,bin,guest,host,true)){
            std::fprintf(stderr,"cannot install %s\n", host);
            std::fclose(f);
            return 1;
        }
    }

    if(!save_inode_table(f,ins)||!save_bitmap(f,bm)){std::fprintf(stderr,"cannot write metadata\n");std::fclose(f);return 1;}
    std::fflush(f);std::fclose(f);
    std::printf("persistent filesystem ready: %s (%d apps)\n",img, argc-2);
    return 0;
}