#ifndef FAT32_H
#define FAT32_H

#include "types.h"

/* FAT32 volumes on a raw device (storage_dev_* numbers: 1/5 RAM disks, 3 NVMe, 4 AHCI). Not mounted into the VFS;
   accessed through the `fat` command (list/cat/get/put/mkdir/rm/mkfs). Long file names are read; new names must be 8.3. */
struct FatInfo { uint64_t total_sectors; uint32_t sectors_per_cluster; uint32_t clusters; uint32_t free_clusters; uint32_t root_cluster; char label[12]; };
typedef void (*FatListCb)(const char* name, uint32_t size, int is_dir, void* ctx);

int fat_mkfs(int dev, const char* label);                          /* 0 ok */
int fat_info(int dev, FatInfo* out);                               /* 0 ok, -1 not FAT32 */
int fat_list(int dev, const char* path, FatListCb cb, void* ctx);  /* 0 ok, -2 not found, -20 not a dir */
int fat_read(int dev, const char* path, uint8_t* buf, uint32_t cap, uint32_t* size);   /* 0 ok, -27 too big */
int fat_write(int dev, const char* path, const uint8_t* data, uint32_t size);          /* create or replace */
int fat_mkdir(int dev, const char* path);
int fat_remove(int dev, const char* path);                         /* file or empty directory */

#endif
