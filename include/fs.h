#ifndef FS_H
#define FS_H

#include "types.h"

enum FsNodeType
{
    FS_FILE = 0,
    FS_DIR = 1,
    FS_SYMLINK = 2
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

struct FsJournalInfo { uint32_t enabled; uint64_t start_sector; uint64_t sectors; uint32_t commits; uint32_t replayed; };
void fs_init();
int fs_resolve_nofollow(const char* path, int cwd);
int fs_symlink(const char* target, const char* linkpath, int cwd);   /* returns node id or -errno */
int fs_link_target_of(int id, char* out, int cap);                     /* target of a symlink node */
int fs_readlink(const char* path, int cwd, char* out, int cap);       /* length or -errno */
void fs_set_readonly(int ro);                                         /* remount ro/rw; mutators return -30 when ro */
int fs_is_readonly();
int fs_sync();
void fs_journal_status(FsJournalInfo* out);
int fs_journal_crash_test(const char* path, int cwd);
void fs_set_credentials(uint32_t uid, uint32_t gid);
uint32_t fs_current_uid();
uint32_t fs_current_gid();
int fs_check_access(int id, uint32_t requested);
int fs_check_access_as(int id, uint32_t uid, uint32_t gid, uint32_t requested);
int fs_stat_path(const char* path, int cwd, FsStat* out);
int fs_lstat_path(const char* path, int cwd, FsStat* out);   /* like stat but does not follow a final symbolic link */
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

/* ---- multiple mounts ----
   Node ids are global: (mount index << 12) | inode index (mount 0 = root). Storage device numbers (storage_dev_*):
   1 = RAM disk 0, 5 = RAM disk 1 (tmpfs), 3 = NVMe, 4 = AHCI/SATA when not the root disk. */
/* hard links, fsck, quota */
struct FsckInfo { int inodes, dirs, files, symlinks, hardlinks; int structure_ok, bitmap_ok, repaired; uint64_t sectors_used, leaked, missing; };
int fs_link_hard(const char* target, const char* linkpath, int cwd);   /* node id or -errno; same-mount regular files with data only */
int fs_link_count(int id);
int fs_fsck(const char* path, int cwd, int repair, FsckInfo* out);   /* path = a mount root or NULL (root fs) */
int fs_quota_set(uint32_t uid, uint64_t bytes);                         /* 0 removes the limit; persisted in /etc/quota */
int fs_quota_get(uint32_t uid, uint64_t* limit, uint64_t* used);

struct FsMountInfo { int dev; int readonly; int journal; int version; uint64_t total_sectors; uint64_t used_sectors; char dir[96]; };
int fs_mount_count();
int fs_mount_info(int index, FsMountInfo* out);                    /* index 0 = root filesystem */
int fs_mount_device(int dev, const char* path, int cwd, int readonly, int format_fresh);   /* 0 ok, -errno */
int fs_mkfs_device(int dev);                                       /* format a device (must not be mounted) */
int fs_umount_path(const char* path, int cwd, int busy_gid);       /* busy_gid: node id the shell is in, or -1 */

#endif
