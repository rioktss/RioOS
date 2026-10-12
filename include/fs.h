#ifndef FS_H
#define FS_H

#include "types.h"

enum FsNodeType
{
    FS_FILE = 0,
    FS_DIR = 1
};

enum FsAccessBits
{
    FS_ACCESS_EXEC  = 1U,
    FS_ACCESS_WRITE = 2U,
    FS_ACCESS_READ  = 4U
};

struct FsStat
{
    uint32_t inode;
    uint32_t type;
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint32_t sector_count;
    uint64_t size;
    uint64_t mtime;
};

void fs_init();
void fs_set_credentials(uint32_t uid, uint32_t gid);
uint32_t fs_current_uid();
uint32_t fs_current_gid();
int fs_check_access(int id, uint32_t requested);
int fs_check_access_as(int id, uint32_t uid, uint32_t gid, uint32_t requested);
int fs_stat_path(const char* path, int cwd, FsStat* out);
int fs_set_owner(int id, uint32_t uid, uint32_t gid);
int fs_set_owner_path(const char* path, int cwd, uint32_t uid, uint32_t gid);
int fs_truncate(const char* path, int cwd, uint64_t size);
int fs_get_root();
int fs_resolve(const char* path, int cwd);
int fs_mkdir(const char* path, int cwd);
int fs_touch(const char* path, int cwd);
int fs_write(const char* path, int cwd, const char* content);
int fs_write_data(const char* path, int cwd, const uint8_t* data, uint64_t size);
int fs_read_file(const char* path, int cwd, uint8_t* buffer, uint64_t capacity, uint64_t* out_size);
/* Kernel-internal update helper: exposes content only when caller has WRITE access.
   Never wire this API directly to an EL0 syscall. */
int fs_read_file_for_update(const char* path, int cwd, uint8_t* buffer, uint64_t capacity, uint64_t* out_size);
int fs_cat(const char* path, int cwd);
int fs_cat_numbered(const char* path, int cwd);
int fs_rm(const char* path, int cwd);
int fs_rm_recursive(const char* path, int cwd);
/* Move/rename. 0 ok; -1 source missing; -2 destination exists; -3 root;
   -4 into itself; -5 bad destination; -6 I/O error. */
int fs_rename(const char* from, const char* to, int cwd);
int fs_mkdir_p(const char* path, int cwd);
uint64_t fs_file_size(int id);
int fs_get_type(int id);
const char* fs_get_name(int id);
int fs_get_parent(int id);
uint32_t fs_get_mode(int id);
uint64_t fs_get_mtime(int id);
uint32_t fs_get_uid(int id);
uint32_t fs_get_gid(int id);
int fs_set_mode(int id, uint32_t mode);
int fs_set_mode_path(const char* path, int cwd, uint32_t mode);
uint64_t fs_total_sectors();
uint64_t fs_used_sectors();
int fs_get_child_count(int id);
int fs_get_child(int id, int index);
int fs_persistent();
#endif
