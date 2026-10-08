#ifndef FS_H
#define FS_H

#include "types.h"

enum FsNodeType
{
    FS_FILE = 0,
    FS_DIR = 1
};

void fs_init();
int fs_get_root();
int fs_resolve(const char* path, int cwd);
int fs_mkdir(const char* path, int cwd);
int fs_touch(const char* path, int cwd);
int fs_write(const char* path, int cwd, const char* content);
int fs_write_data(const char* path, int cwd, const uint8_t* data, uint64_t size);
int fs_read_file(const char* path, int cwd, uint8_t* buffer, uint64_t capacity, uint64_t* out_size);
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
int fs_set_mode(int id, uint32_t mode);
int fs_set_mode_path(const char* path, int cwd, uint32_t mode);
uint64_t fs_total_sectors();
uint64_t fs_used_sectors();
int fs_get_child_count(int id);
int fs_get_child(int id, int index);
int fs_persistent();
#endif
