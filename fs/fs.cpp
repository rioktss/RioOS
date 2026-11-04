#include "fs.h"
#include "storage.h"
#include "uart.h"
#include "string.h"
#include "rtc.h"

/* Every public fs_* entry point below is the single-mount implementation (*_impl, local ids); the real API at the end of
   this file routes calls to the right mounted filesystem and encodes ids as (mount << 12) | local id. */
#define fs_check_access_as fs_check_access_as_impl
#define fs_check_access fs_check_access_impl
#define fs_init fs_init_impl
#define fs_get_root fs_get_root_impl
#define fs_resolve fs_resolve_impl
#define fs_resolve_nofollow fs_resolve_nofollow_impl
#define fs_symlink fs_symlink_impl
#define fs_readlink fs_readlink_impl
#define fs_link_target_of fs_link_target_of_impl
#define fs_set_readonly fs_set_readonly_impl
#define fs_is_readonly fs_is_readonly_impl
#define fs_sync fs_sync_impl
#define fs_journal_status fs_journal_status_impl
#define fs_journal_crash_test fs_journal_crash_test_impl
#define fs_mkdir fs_mkdir_impl
#define fs_touch fs_touch_impl
#define fs_mkdir_p fs_mkdir_p_impl
#define fs_write_data fs_write_data_impl
#define fs_write fs_write_impl
#define fs_read_file fs_read_file_impl
#define fs_read_file_for_update fs_read_file_for_update_impl
#define fs_cat fs_cat_impl
#define fs_cat_numbered fs_cat_numbered_impl
#define fs_rm fs_rm_impl
#define fs_rm_recursive fs_rm_recursive_impl
#define fs_rename fs_rename_impl
#define fs_file_size fs_file_size_impl
#define fs_get_type fs_get_type_impl
#define fs_get_name fs_get_name_impl
#define fs_get_parent fs_get_parent_impl
#define fs_get_child_count fs_get_child_count_impl
#define fs_get_child fs_get_child_impl
#define fs_get_mode fs_get_mode_impl
#define fs_get_mtime fs_get_mtime_impl
#define fs_get_uid fs_get_uid_impl
#define fs_get_gid fs_get_gid_impl
#define fs_set_mode fs_set_mode_impl
#define fs_set_owner fs_set_owner_impl
#define fs_set_owner_path fs_set_owner_path_impl
#define fs_stat_path fs_stat_path_impl
#define fs_truncate fs_truncate_impl
#define fs_total_sectors fs_total_sectors_impl
#define fs_used_sectors fs_used_sectors_impl
#define fs_set_mode_path fs_set_mode_path_impl
#define fs_persistent fs_persistent_impl
#define fs_link_hard fs_link_hard_impl
#define fs_link_count fs_link_count_impl
#define fs_fsck fs_fsck_impl
#define fs_quota_set fs_quota_set_impl
#define fs_quota_get fs_quota_get_impl
/* forward declarations of the single-mount implementations (names redirected by the macros above) */
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
int fs_read_file_for_update(const char* path, int cwd, uint8_t* buffer, uint64_t capacity, uint64_t* out_size);
int fs_cat(const char* path, int cwd);
int fs_cat_numbered(const char* path, int cwd);
int fs_rm(const char* path, int cwd);
int fs_rm_recursive(const char* path, int cwd);
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

static uint64_t rtc_now()
{
    return rtc_get_epoch();
}

#define FS_MAGIC            0x4D4B4653U
#define FS_META_TAG         0x52494F53U /* "RIOS" metadata extension marker */
#define FS_VERSION_V1       1U
#define FS_VERSION          2U   /* v2: 256 inodes, 4 MiB files; v1 (64 inodes) images still mount */
#define FS_SECTOR_SIZE      512ULL
#define FS_NODE_CAP         256
#define FS_NAME_SIZE        64
#define FS_INODE_SIZE       128
#define FS_BITMAP_SECTORS   8
#define FS_INODE_START      1ULL
#define FS_MAX_FILE_SIZE    (4096ULL * 1024ULL)
#define FS_MAX_NODES        node_count
#define FS_INODE_SECTORS    g_inode_sectors
#define FS_BITMAP_START     g_bitmap_start
#define FS_DATA_START       g_data_start
#define FS_MAX_DISK_SECTORS 32768ULL
#define FS_DEFAULT_DISK_SECTORS 32768ULL
#define FS_MAX_BITMAP_BYTES (FS_MAX_DISK_SECTORS / 8ULL)

struct __attribute__((packed)) DiskSuper
{
    uint32_t magic;
    uint32_t version;
    uint32_t sector_size;
    uint32_t inode_size;
    uint32_t inode_count;
    uint32_t inode_start;
    uint32_t inode_sectors;
    uint32_t bitmap_start;
    uint32_t bitmap_sectors;
    uint32_t data_start;
    uint64_t total_sectors;
    uint8_t reserved[512 - 48];
};

struct __attribute__((packed)) DiskInode
{
    uint32_t used;
    uint32_t type;
    int32_t parent;
    uint32_t flags;
    uint64_t size;
    uint64_t first_sector;
    uint32_t sector_count;
    char name[FS_NAME_SIZE];
    uint8_t reserved[28];
};

struct FsNode
{
    int used;
    int type;
    int parent;
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint64_t mtime;
    uint64_t size;
    uint64_t first_sector;
    uint32_t sector_count;
    char name[FS_NAME_SIZE];
    int mount_ctx;                      /* >0: another filesystem is mounted on this directory (in-memory only) */
};

/* ---- per-mount context: every mounted filesystem has its own inode table, bitmap, journal and device ---- */
#define FS_MAX_CTX 8
struct FsCtx
{
    FsNode nodes[FS_NODE_CAP];
    uint8_t bitmap[FS_MAX_BITMAP_BYTES];
    uint8_t legacy_metadata[FS_NODE_CAP];
    uint64_t disk_sectors;
    int root_id, mounted, node_count;
    uint64_t g_inode_sectors, g_bitmap_start, g_data_start;
    uint32_t g_version;
    int jnl_enabled, fs_ro;
    uint32_t jnl_seq;
    int inuse, dev, quiet, no_journal;
    uint64_t fmt_sectors;
    int mp_ctx, mp_local;
};
static FsCtx ctxs[FS_MAX_CTX];
static FsCtx* cur = &ctxs[0];
#define nodes           (cur->nodes)
#define bitmap          (cur->bitmap)
#define legacy_metadata (cur->legacy_metadata)
#define disk_sectors    (cur->disk_sectors)
#define root_id         (cur->root_id)
#define mounted         (cur->mounted)
#define node_count      (cur->node_count)
#define g_inode_sectors (cur->g_inode_sectors)
#define g_bitmap_start  (cur->g_bitmap_start)
#define g_data_start    (cur->g_data_start)
#define g_version       (cur->g_version)
#define jnl_enabled     (cur->jnl_enabled)
#define fs_ro           (cur->fs_ro)
#define jnl_seq         (cur->jnl_seq)
static void set_layout(uint32_t version)
{
    g_version = version;
    node_count = version == FS_VERSION_V1 ? 64 : 256;
    g_inode_sectors = (uint64_t)node_count * 128ULL / 512ULL;
    g_bitmap_start = 1ULL + g_inode_sectors;
    g_data_start = g_bitmap_start + 8ULL;
}
static int fs_verbose = 1;
#define fs_say(s) do { if (fs_verbose && !cur->quiet) uart_puts(s); } while (0)

/* Device routing: device 0 = the root storage backend (storage_*); others through storage_dev_* (weak: absent in host tests). */
__attribute__((weak)) int storage_dev_read(int dev, uint64_t sector, void* buffer);
__attribute__((weak)) int storage_dev_write(int dev, uint64_t sector, const void* buffer);
__attribute__((weak)) int storage_dev_flush(int dev);
__attribute__((weak)) uint64_t storage_dev_sectors(int dev);
static int dev_read(uint64_t sector, void* buf) { return cur->dev == 0 ? storage_read_sector(sector, buf) : (storage_dev_read ? storage_dev_read(cur->dev, sector, buf) : -1); }
static int dev_write(uint64_t sector, const void* buf) { return cur->dev == 0 ? storage_write_sector(sector, buf) : (storage_dev_write ? storage_dev_write(cur->dev, sector, buf) : -1); }
static int dev_flush() { return cur->dev == 0 ? storage_flush() : (storage_dev_flush ? storage_dev_flush(cur->dev) : -1); }
static int dev_ready() { return cur->dev == 0 ? storage_ready() : (storage_dev_sectors ? storage_dev_sectors(cur->dev) != 0 : 0); }
#define storage_read_sector(s, b)  dev_read((s), (b))
#define storage_write_sector(s, b) dev_write((s), (b))
#define storage_flush()            dev_flush()
#define storage_ready()            dev_ready()

static uint8_t io_buffer[FS_SECTOR_SIZE] __attribute__((aligned(16)));
static uint32_t current_uid = 0;
static uint32_t current_gid = 0;
static uint8_t truncate_buffer[FS_MAX_FILE_SIZE];
static void clear_bytes(void* ptr, uint64_t size);

/* ---- Write-ahead metadata journal (ordered mode) --------------------------
   File data goes straight to its (new) extent first; every metadata sector
   (inode table, allocation bitmap, superblock) touched by one operation is
   staged in RAM, then committed atomically: journal data -> journal header
   (commit record) -> in-place apply -> header cleared. Mount replays a
   committed-but-unapplied transaction. Journal = last 33 sectors of the disk. */
#define JNL_SECTORS 33ULL
#define JNL_MAX     32
#define JNL_MAGIC   0x4A4E4C52U
struct __attribute__((packed)) JnlHeader
{
    uint32_t magic;
    uint32_t seq;
    uint32_t count;
    uint32_t checksum;
    uint64_t target[JNL_MAX];
};
static int tx_depth = 0;
static int tx_count = 0;
static int jnl_crash_armed = 0;
static uint32_t jnl_commits = 0;
static uint32_t jnl_replayed = 0;
static uint64_t tx_sector[JNL_MAX];
static uint8_t tx_data[JNL_MAX][512] __attribute__((aligned(16)));
static uint8_t jnl_io[512] __attribute__((aligned(16)));
static uint64_t jnl_start() { return disk_sectors - JNL_SECTORS; }

static int meta_write(uint64_t sector, const void* buf)
{
    if (jnl_enabled && tx_depth > 0)
    {
        int slot = -1;
        for (int i = 0; i < tx_count; ++i) if (tx_sector[i] == sector) { slot = i; break; }
        if (slot < 0)
        {
            if (tx_count >= JNL_MAX) return -1;
            slot = tx_count++;
            tx_sector[slot] = sector;
        }
        for (int i = 0; i < 512; ++i) tx_data[slot][i] = ((const uint8_t*)buf)[i];
        return 0;
    }
    return storage_write_sector(sector, buf);
}

static int meta_read(uint64_t sector, void* buf)
{
    if (jnl_enabled && tx_depth > 0)
        for (int i = 0; i < tx_count; ++i)
            if (tx_sector[i] == sector)
            {
                for (int k = 0; k < 512; ++k) ((uint8_t*)buf)[k] = tx_data[i][k];
                return 0;
            }
    return storage_read_sector(sector, buf);
}

static int meta_flush() { return tx_depth > 0 ? 0 : storage_flush(); }

static uint32_t jnl_sum(const JnlHeader* h, const uint8_t (*data)[512])
{
    uint32_t x = 2166136261U;
    for (uint32_t i = 0; i < h->count; ++i)
    {
        for (int b = 0; b < 8; ++b) { x ^= (uint8_t)(h->target[i] >> (b * 8)); x *= 16777619U; }
        for (int b = 0; b < 512; ++b) { x ^= data[i][b]; x *= 16777619U; }
    }
    x ^= h->seq; x *= 16777619U;
    x ^= h->count; x *= 16777619U;
    return x;
}

static int jnl_clear_header()
{
    clear_bytes(jnl_io, 512);
    if (storage_write_sector(jnl_start(), jnl_io) != 0) return -1;
    return storage_flush();
}

static void tx_end()
{
    if (--tx_depth > 0) return;
    if (tx_depth < 0) tx_depth = 0;
    if (!jnl_enabled || tx_count == 0) { tx_count = 0; return; }
    int n = tx_count;
    tx_count = 0;
    uint64_t js = jnl_start();
    for (int i = 0; i < n; ++i)
        if (storage_write_sector(js + 1 + (uint64_t)i, tx_data[i]) != 0) return;
    if (storage_flush() != 0) return;
    JnlHeader h;
    clear_bytes(&h, sizeof(h));
    h.magic = JNL_MAGIC; h.seq = ++jnl_seq; h.count = (uint32_t)n;
    for (int i = 0; i < n; ++i) h.target[i] = tx_sector[i];
    h.checksum = jnl_sum(&h, tx_data);
    clear_bytes(jnl_io, 512);
    for (uint64_t i = 0; i < sizeof(h); ++i) jnl_io[i] = ((const uint8_t*)&h)[i];
    if (storage_write_sector(js, jnl_io) != 0) return;
    if (storage_flush() != 0) return;          /* commit point */
    ++jnl_commits;
    if (jnl_crash_armed) { jnl_crash_armed = 0; return; }   /* simulated power loss before apply */
    for (int i = 0; i < n; ++i)
        if (storage_write_sector(tx_sector[i], tx_data[i]) != 0) return;
    if (storage_flush() != 0) return;
    (void)jnl_clear_header();
}

struct FsTx { FsTx() { ++tx_depth; } ~FsTx() { tx_end(); } };
#define FS_RW_CHECK() do { if (fs_ro) return -30; } while (0)

static int jnl_recover()
{
    if (!jnl_enabled) return 0;
    uint64_t js = jnl_start();
    if (storage_read_sector(js, jnl_io) != 0) return -1;
    JnlHeader h;
    for (uint64_t i = 0; i < sizeof(h); ++i) ((uint8_t*)&h)[i] = jnl_io[i];
    if (h.magic != JNL_MAGIC) return 0;
    if (h.count == 0 || h.count > JNL_MAX) return jnl_clear_header();
    static uint8_t rdata[JNL_MAX][512] __attribute__((aligned(16)));
    for (uint32_t i = 0; i < h.count; ++i)
        if (storage_read_sector(js + 1 + i, rdata[i]) != 0) return -1;
    if (jnl_sum(&h, rdata) != h.checksum)       /* torn commit: discard */
        return jnl_clear_header();
    for (uint32_t i = 0; i < h.count; ++i)
    {
        if (h.target[i] >= js) continue;          /* never write into the journal itself */
        if (storage_write_sector(h.target[i], rdata[i]) != 0) return -1;
    }
    if (storage_flush() != 0) return -1;
    jnl_seq = h.seq;
    jnl_replayed += h.count;
    return jnl_clear_header();
}
static uint32_t default_mode_for_type(int type)
{
    return type == FS_DIR ? 0755U : 0644U;
}

static void set_mode_and_time_defaults(FsNode* n)
{
    if (n == 0) return;
    if (n->mode == 0) n->mode = default_mode_for_type(n->type);
    if (n->mtime == 0) n->mtime = rtc_now();
}

static void ram_fallback()
{
    mounted = 0;
    root_id = 0;
    for (int i = 0; i < FS_MAX_NODES; ++i)
        clear_bytes(&nodes[i], sizeof(nodes[i]));
    nodes[0].used = 1;
    nodes[0].type = FS_DIR;
    nodes[0].parent = -1;
    nodes[0].mode = 0755U;
    nodes[0].uid = 0; nodes[0].gid = 0;
    nodes[0].mtime = rtc_now();
    str_copy(nodes[0].name, "/", FS_NAME_SIZE);
}


static void clear_bytes(void* ptr, uint64_t size)
{
    uint8_t* p = (uint8_t*)ptr;
    for (uint64_t i = 0; i < size; ++i)
        p[i] = 0;
}

static void copy_node(FsNode* dst, const FsNode* src)
{
    if (!dst || !src) return;
    dst->used = src->used;
    dst->type = src->type;
    dst->parent = src->parent;
    dst->mode = src->mode;
    dst->uid = src->uid;
    dst->gid = src->gid;
    dst->mtime = src->mtime;
    dst->size = src->size;
    dst->first_sector = src->first_sector;
    dst->sector_count = src->sector_count;
    for (int i = 0; i < FS_NAME_SIZE; ++i)
        dst->name[i] = src->name[i];
}

static int bitmap_get(uint64_t sector)
{
    if (sector >= FS_MAX_DISK_SECTORS)
        return 0;
    return (bitmap[sector >> 3] >> (sector & 7ULL)) & 1U;
}

static void bitmap_set(uint64_t sector, int value)
{
    if (sector >= FS_MAX_DISK_SECTORS)
        return;
    uint8_t mask = (uint8_t)(1U << (sector & 7ULL));
    if (value)
        bitmap[sector >> 3] |= mask;
    else
        bitmap[sector >> 3] &= (uint8_t)~mask;
}

static int write_bitmap()
{
    for (uint64_t s = 0; s < FS_BITMAP_SECTORS; ++s)
    {
        const uint8_t* src = bitmap + s * FS_SECTOR_SIZE;
        if (meta_write(FS_BITMAP_START + s, src) != 0)
            return -1;
    }
    return meta_flush();
}

static void node_to_disk(const FsNode* n, DiskInode* d)
{
    clear_bytes(d, sizeof(*d));
    d->used = n->used ? 1U : 0U;
    d->type = (uint32_t)n->type;
    d->parent = n->parent;
    d->size = n->size;
    d->first_sector = n->first_sector;
    d->sector_count = n->sector_count;
    for (int i = 0; i < 4; ++i)
        d->reserved[i] = (uint8_t)((n->mode >> (i * 8)) & 0xFFU);
    for (int i = 0; i < 8; ++i)
        d->reserved[4 + i] = (uint8_t)((n->mtime >> (i * 8)) & 0xFFULL);
    for (int i = 0; i < 4; ++i)
    {
        d->reserved[12 + i] = (uint8_t)((n->uid >> (i * 8)) & 0xFFU);
        d->reserved[16 + i] = (uint8_t)((n->gid >> (i * 8)) & 0xFFU);
        d->reserved[20 + i] = (uint8_t)((FS_META_TAG >> (i * 8)) & 0xFFU);
    }
    str_copy(d->name, n->name, FS_NAME_SIZE);
}

static void disk_to_node(const DiskInode* d, FsNode* n)
{
    clear_bytes(n, sizeof(*n));
    n->used = d->used ? 1 : 0;
    n->type = (int)d->type;
    n->parent = d->parent;
    n->size = d->size;
    n->first_sector = d->first_sector;
    n->sector_count = d->sector_count;
    n->mode = 0;
    for (int i = 0; i < 4; ++i)
        n->mode |= (uint32_t)d->reserved[i] << (i * 8);
    n->mtime = 0;
    for (int i = 0; i < 8; ++i)
        n->mtime |= (uint64_t)d->reserved[4 + i] << (i * 8);
    if (n->mode == 0) n->mode = default_mode_for_type(n->type);
    uint32_t tag = 0;
    for (int i = 0; i < 4; ++i)
    {
        n->uid |= (uint32_t)d->reserved[12 + i] << (i * 8);
        n->gid |= (uint32_t)d->reserved[16 + i] << (i * 8);
        tag |= (uint32_t)d->reserved[20 + i] << (i * 8);
    }
    /* Old v1 images had no owner fields. They migrate non-root nodes to the
       standard unprivileged account (1000:1000); inode 0 is fixed to root. */
    n->uid = tag == FS_META_TAG ? n->uid : 1000U;
    n->gid = tag == FS_META_TAG ? n->gid : 1000U;
    str_copy(n->name, d->name, FS_NAME_SIZE);
}

static int save_inode(int id)
{
    if (id < 0 || id >= FS_MAX_NODES)
        return -1;
    uint64_t index = (uint64_t)id;
    uint64_t sector = FS_INODE_START + (index * FS_INODE_SIZE) / FS_SECTOR_SIZE;
    uint64_t offset = (index * FS_INODE_SIZE) % FS_SECTOR_SIZE;
    if (meta_read(sector, io_buffer) != 0)
        return -1;
    DiskInode d;
    node_to_disk(&nodes[id], &d);
    for (uint64_t i = 0; i < sizeof(d); ++i)
        io_buffer[offset + i] = ((const uint8_t*)&d)[i];
    if (meta_write(sector, io_buffer) != 0)
        return -1;
    return meta_flush();
}

static int save_super()
{
    DiskSuper sb;
    clear_bytes(&sb, sizeof(sb));
    sb.magic = FS_MAGIC;
    sb.version = g_version;
    sb.sector_size = FS_SECTOR_SIZE;
    sb.inode_size = FS_INODE_SIZE;
    sb.inode_count = FS_MAX_NODES;
    sb.inode_start = FS_INODE_START;
    sb.inode_sectors = FS_INODE_SECTORS;
    sb.bitmap_start = FS_BITMAP_START;
    sb.bitmap_sectors = FS_BITMAP_SECTORS;
    sb.data_start = FS_DATA_START;
    sb.total_sectors = disk_sectors;
    clear_bytes(io_buffer, FS_SECTOR_SIZE);
    for (uint64_t i = 0; i < sizeof(sb); ++i)
        io_buffer[i] = ((const uint8_t*)&sb)[i];
    if (meta_write(0, io_buffer) != 0)
        return -1;
    return meta_flush();
}

static int load_super()
{
    if (storage_read_sector(0, io_buffer) != 0)
        return -1;
    DiskSuper sb;
    for (uint64_t i = 0; i < sizeof(sb); ++i)
        ((uint8_t*)&sb)[i] = io_buffer[i];
    if (sb.magic != FS_MAGIC || (sb.version != FS_VERSION && sb.version != FS_VERSION_V1)) return -2;
    set_layout(sb.version);
    if (sb.sector_size != FS_SECTOR_SIZE || sb.inode_size != FS_INODE_SIZE ||
        sb.inode_count != (uint32_t)FS_MAX_NODES ||
        sb.inode_start != FS_INODE_START || sb.inode_sectors != FS_INODE_SECTORS ||
        sb.bitmap_start != FS_BITMAP_START || sb.bitmap_sectors != FS_BITMAP_SECTORS ||
        sb.data_start != FS_DATA_START)
        return -2;
    if (sb.total_sectors < FS_DATA_START || sb.total_sectors > FS_MAX_DISK_SECTORS)
        return -3;
    disk_sectors = sb.total_sectors;
    jnl_enabled = !cur->no_journal && disk_sectors >= FS_DATA_START + 256ULL;
    return 0;
}

static int load_all_inodes()
{
    for (uint64_t s = 0; s < FS_INODE_SECTORS; ++s)
    {
        if (storage_read_sector(FS_INODE_START + s, io_buffer) != 0)
            return -1;
        for (int slot = 0; slot < 4; ++slot)
        {
            DiskInode d;
            uint64_t offset = (uint64_t)slot * FS_INODE_SIZE;
            for (uint64_t i = 0; i < sizeof(d); ++i)
                ((uint8_t*)&d)[i] = io_buffer[offset + i];
            int node_id = (int)(s * 4ULL + slot);
            uint32_t tag = 0;
            for (int k = 0; k < 4; ++k)
                tag |= (uint32_t)d.reserved[20 + k] << (k * 8);
            legacy_metadata[node_id] = tag != FS_META_TAG;
            disk_to_node(&d, &nodes[node_id]);
            if (node_id == 0) { nodes[node_id].uid = 0; nodes[node_id].gid = 0; }
            if (nodes[node_id].used)
                set_mode_and_time_defaults(&nodes[(int)(s * 4ULL + slot)]);
        }
    }
    return 0;
}

static void reserve_metadata();

static int rebuild_bitmap_from_inodes()
{
    clear_bytes(bitmap, sizeof(bitmap));
    reserve_metadata();

    for (int i = 0; i < FS_MAX_NODES; ++i)
    {
        if (!nodes[i].used || (nodes[i].type != FS_FILE && nodes[i].type != FS_SYMLINK) || nodes[i].sector_count == 0)
            continue;

        /* Validate the start before subtracting it from disk_sectors. The
           subtraction is unsigned and would underflow for a corrupt extent
           whose first sector lies past the end of the device. */
        if (nodes[i].first_sector < FS_DATA_START ||
            nodes[i].first_sector >= disk_sectors ||
            nodes[i].sector_count > disk_sectors - nodes[i].first_sector ||
            nodes[i].sector_count > FS_MAX_FILE_SIZE / FS_SECTOR_SIZE)
            return -1;

        /* hard links share one extent: only the first holder marks it */
        int dup = 0;
        for (int j = 0; j < i; ++j)
            if (nodes[j].used && nodes[j].type == FS_FILE && nodes[i].type == FS_FILE &&
                nodes[j].first_sector == nodes[i].first_sector && nodes[j].sector_count == nodes[i].sector_count) { dup = 1; break; }
        if (dup) continue;

        for (uint32_t s = 0; s < nodes[i].sector_count; ++s)
        {
            uint64_t sector = nodes[i].first_sector + s;
            if (bitmap_get(sector))
                return -2;
            bitmap_set(sector, 1);
        }
    }
    return 0;
}

static int validate_mounted_fs()
{
    if (!nodes[0].used || nodes[0].type != FS_DIR || nodes[0].parent != -1 ||
        !str_equal(nodes[0].name, "/"))
        return 0;

    for (int i = 0; i < FS_MAX_NODES; ++i)
    {
        if (!nodes[i].used)
            continue;
        if (i != 0)
        {
            if (nodes[i].parent < 0 || nodes[i].parent >= FS_MAX_NODES ||
                !nodes[nodes[i].parent].used || nodes[nodes[i].parent].type != FS_DIR)
                return 0;
            if (nodes[i].name[0] == '\0' || str_len(nodes[i].name) >= FS_NAME_SIZE ||
                str_equal(nodes[i].name, ".") || str_equal(nodes[i].name, ".."))
                return 0;
            /* A stored name is one path component, never another path. */
            for (int k = 0; nodes[i].name[k] != '\0'; ++k)
                if (nodes[i].name[k] == '/') return 0;
        }

        if ((nodes[i].mode & ~0777U) != 0 || nodes[i].uid > 0x7FFFFFFFU ||
            nodes[i].gid > 0x7FFFFFFFU)
            return 0;

        if (nodes[i].type == FS_FILE || nodes[i].type == FS_SYMLINK)
        {
            if (nodes[i].type == FS_SYMLINK && (nodes[i].size == 0 || nodes[i].size > 255)) return 0;
            /* Bound size before adding sector_size - 1; otherwise a corrupt
               UINT64_MAX size can wrap and appear to need zero sectors. */
            if (nodes[i].size > FS_MAX_FILE_SIZE)
                return 0;
            uint64_t needed = (nodes[i].size + FS_SECTOR_SIZE - 1ULL) / FS_SECTOR_SIZE;
            if (needed != nodes[i].sector_count || nodes[i].sector_count > FS_MAX_FILE_SIZE / FS_SECTOR_SIZE)
                return 0;
            if (needed == 0)
            {
                if (nodes[i].first_sector != 0)
                    return 0;
            }
            else
            {
                if (nodes[i].first_sector < FS_DATA_START ||
                    nodes[i].first_sector >= disk_sectors ||
                    nodes[i].sector_count > disk_sectors - nodes[i].first_sector)
                    return 0;
                for (uint32_t s = 0; s < nodes[i].sector_count; ++s)
                    if (!bitmap_get(nodes[i].first_sector + s))
                        return 0;
            }
        }
        else if (nodes[i].type == FS_DIR)
        {
            if (nodes[i].size != 0 || nodes[i].first_sector != 0 ||
                nodes[i].sector_count != 0)
                return 0;
        }
        else
        {
            return 0;
        }
    }

    /* Reject duplicate sibling names; path lookup must resolve exactly one
       entry for a given (parent, name) pair. */
    for (int a = 1; a < FS_MAX_NODES; ++a)
    {
        if (!nodes[a].used) continue;
        for (int b = a + 1; b < FS_MAX_NODES; ++b)
            if (nodes[b].used && nodes[a].parent == nodes[b].parent &&
                str_equal(nodes[a].name, nodes[b].name))
                return 0;
    }

    /* Detect duplicated/overlapping allocated extents between files. */
    for (int a = 0; a < FS_MAX_NODES; ++a)
    {
        if (!nodes[a].used || nodes[a].type == FS_DIR || nodes[a].sector_count == 0)
            continue;
        uint64_t a0 = nodes[a].first_sector;
        uint64_t a1 = a0 + nodes[a].sector_count;
        for (int b = a + 1; b < FS_MAX_NODES; ++b)
        {
            if (!nodes[b].used || nodes[b].type == FS_DIR || nodes[b].sector_count == 0)
                continue;
            uint64_t b0 = nodes[b].first_sector;
            uint64_t b1 = b0 + nodes[b].sector_count;
            if (a0 == b0 && a1 == b1 && nodes[a].type == FS_FILE && nodes[b].type == FS_FILE && nodes[a].size == nodes[b].size)
                continue;                                   /* hard links */
            if (a0 < b1 && b0 < a1)
                return 0;
        }
    }

    /* Detect parent cycles before pwd/tree can recurse indefinitely. */
    for (int i = 1; i < FS_MAX_NODES; ++i)
    {
        if (!nodes[i].used)
            continue;
        int p = i;
        for (int depth = 0; depth < FS_MAX_NODES && p != 0; ++depth)
            p = nodes[p].parent;
        if (p != 0)
            return 0;
    }
    return 1;
}

static void reserve_metadata()
{
    for (uint64_t s = 0; s < FS_DATA_START && s < FS_MAX_DISK_SECTORS; ++s)
        bitmap_set(s, 1);
    if (jnl_enabled)
        for (uint64_t s = disk_sectors - JNL_SECTORS; s < disk_sectors && s < FS_MAX_DISK_SECTORS; ++s)
            bitmap_set(s, 1);
}

static int format_fs()
{
    set_layout(FS_VERSION);
    disk_sectors = cur->fmt_sectors ? cur->fmt_sectors : FS_DEFAULT_DISK_SECTORS;
    jnl_enabled = !cur->no_journal && disk_sectors >= FS_DATA_START + 256ULL;
    fs_say("FS: formatting persistent filesystem...\r\n");
    clear_bytes(bitmap, sizeof(bitmap));
    for (int i = 0; i < FS_MAX_NODES; ++i)
        clear_bytes(&nodes[i], sizeof(nodes[i]));
    reserve_metadata();

    if (save_super() != 0)
        return -1;
    if (write_bitmap() != 0)
        return -1;

    root_id = 0;
    nodes[0].used = 1;
    nodes[0].type = FS_DIR;
    nodes[0].parent = -1;
    nodes[0].mode = 0755U;
    nodes[0].uid = 0; nodes[0].gid = 0;
    nodes[0].mtime = rtc_now();
    str_copy(nodes[0].name, "/", FS_NAME_SIZE);
    if (save_inode(0) != 0)
        return -1;

    for (int i = 1; i < FS_MAX_NODES; ++i)
        nodes[i].used = 0;

    mounted = 1;
    fs_say("FS: persistent filesystem ready.\r\n");
    return 0;
}

static int find_child(int parent, const char* name)
{
    if (parent < 0 || parent >= FS_MAX_NODES || name == 0)
        return -1;
    if (!nodes[parent].used || nodes[parent].type != FS_DIR)
        return -1;
    for (int i = 0; i < FS_MAX_NODES; ++i)
        if (nodes[i].used && nodes[i].parent == parent && str_equal(nodes[i].name, name))
            return i;
    return -1;
}

static int alloc_node()
{
    for (int i = 0; i < FS_MAX_NODES; ++i)
        if (!nodes[i].used)
            return i;
    return -1;
}

static int alloc_contiguous(uint32_t sectors, uint64_t* out_first)
{
    if (sectors == 0)
    {
        *out_first = 0;
        return 0;
    }
    uint64_t run = 0;
    uint64_t start = FS_DATA_START;
    for (uint64_t s = FS_DATA_START; s < disk_sectors; ++s)
    {
        if (!bitmap_get(s))
            ++run;
        else
            run = 0;
        if (run >= sectors)
        {
            start = s + 1ULL - sectors;
            for (uint64_t k = 0; k < sectors; ++k)
                bitmap_set(start + k, 1);
            *out_first = start;
            return 0;
        }
    }
    return -1;
}

static void free_sectors(uint64_t first, uint32_t count)
{
    if (first < FS_DATA_START)
        return;
    for (uint32_t i = 0; i < count; ++i)
        if (first + i < disk_sectors)
            bitmap_set(first + i, 0);
}

static int create_node(int type, const char* name, int parent)
{
    if (name == 0 || name[0] == '\0' || str_len(name) >= FS_NAME_SIZE)
        return -1;
    if (parent < 0 || parent >= FS_MAX_NODES || !nodes[parent].used || nodes[parent].type != FS_DIR)
        return -1;
    if (find_child(parent, name) >= 0)
        return -2;
    int id = alloc_node();
    if (id < 0)
        return -3;
    clear_bytes(&nodes[id], sizeof(nodes[id]));
    nodes[id].used = 1;
    nodes[id].type = type;
    nodes[id].parent = parent;
    nodes[id].mode = default_mode_for_type(type);
    nodes[id].uid = current_uid;
    nodes[id].gid = current_gid;
    /* Root working inside a user's home keeps that home's owner, so a file
       root creates in /home/user stays editable by the user afterwards. */
    if (current_uid == 0U && nodes[parent].uid != 0U)
    {
        nodes[id].uid = nodes[parent].uid;
        nodes[id].gid = nodes[parent].gid;
    }
    nodes[id].mtime = rtc_now();
    str_copy(nodes[id].name, name, FS_NAME_SIZE);
    if (save_inode(id) != 0)
    {
        nodes[id].used = 0;
        return -4;
    }
    return id;
}

static int create_path(const char* path, int cwd, int type)
{
    if (path == 0 || path[0] == '\0')
        return -1;
    char temp[192];
    if (str_len(path) >= (int)sizeof(temp))
        return -1;
    str_copy(temp, path, sizeof(temp));

    int len = str_len(temp);
    while (len > 1 && temp[len - 1] == '/')
        temp[--len] = '\0';

    int slash = -1;
    for (int i = 0; i < len; ++i)
        if (temp[i] == '/') slash = i;

    int parent = cwd;
    char name[FS_NAME_SIZE];

    if (slash < 0)
    {
        str_copy(name, temp, FS_NAME_SIZE);
    }
    else if (slash == 0)
    {
        parent = root_id;
        str_copy(name, temp + 1, FS_NAME_SIZE);
    }
    else
    {
        char parent_path[192];
        for (int i = 0; i < slash; ++i) parent_path[i] = temp[i];
        parent_path[slash] = '\0';
        parent = fs_resolve(parent_path, cwd);
        str_copy(name, temp + slash + 1, FS_NAME_SIZE);
    }

    if (parent < 0 || name[0] == '\0' || str_equal(name, ".") || str_equal(name, ".."))
        return -1;
    if (fs_check_access(parent, FS_ACCESS_WRITE | FS_ACCESS_EXEC) != 0) return -13;
    return create_node(type, name, parent);
}

static int ensure_dir_path(const char* path)
{
    int id = fs_resolve(path, root_id);
    if (id >= 0)
        return fs_get_type(id) == FS_DIR ? id : -1;
    return fs_mkdir(path, root_id);
}


void fs_set_credentials(uint32_t uid, uint32_t gid)
{
    current_uid = uid;
    current_gid = gid;
}

uint32_t fs_current_uid() { return current_uid; }
uint32_t fs_current_gid() { return current_gid; }

int fs_check_access_as(int id, uint32_t uid, uint32_t gid, uint32_t requested)
{
    if (id < 0 || id >= FS_MAX_NODES || !nodes[id].used) return -2;
    if (requested & ~(FS_ACCESS_READ | FS_ACCESS_WRITE | FS_ACCESS_EXEC)) return -22;
    if (requested == 0 || uid == 0) return 0;
    uint32_t bits;
    if (uid == nodes[id].uid) bits = (nodes[id].mode >> 6) & 7U;
    else if (gid == nodes[id].gid) bits = (nodes[id].mode >> 3) & 7U;
    else bits = nodes[id].mode & 7U;
    return ((bits & requested) == requested) ? 0 : -13;
}

int fs_check_access(int id, uint32_t requested)
{
    return fs_check_access_as(id, current_uid, current_gid, requested);
}

static int fs_parent_writable(int id)
{
    int parent = fs_get_parent(id);
    return parent < 0 ? -13 : fs_check_access(parent, FS_ACCESS_WRITE | FS_ACCESS_EXEC);
}

static int fs_is_descendant_of(int id, int ancestor)
{
    if (id < 0 || ancestor < 0) return 0;
    int current = id;
    for (int depth = 0; depth < FS_MAX_NODES && current >= 0; ++depth)
    {
        if (current == ancestor) return 1;
        if (current == root_id) return 0;
        current = nodes[current].parent;
    }
    return 0;
}

static void fs_assign_legacy_owner_policy()
{
    int user_home = fs_resolve("/home/user", root_id);
    int data_home = fs_resolve("/storage/home", root_id);
    for (int i = 0; i < FS_MAX_NODES; ++i)
    {
        if (!nodes[i].used || !legacy_metadata[i]) continue;
        if (i == root_id)
        {
            nodes[i].uid = 0; nodes[i].gid = 0;
        }
        else if ((user_home >= 0 && fs_is_descendant_of(i, user_home)) ||
                 (data_home >= 0 && fs_is_descendant_of(i, data_home)))
        {
            nodes[i].uid = 1000U; nodes[i].gid = 1000U;
        }
        else
        {
            /* Legacy files outside user homes become system-owned. */
            nodes[i].uid = 0; nodes[i].gid = 0;
        }
    }
}

/* Mount the filesystem found on cur->dev into the current context (cur). allow_format: blank device -> format. */
static void quota_load();
static int mount_current(int allow_format)
{
    root_id = -1;
    mounted = 0;
    disk_sectors = 0;
    for (int i = 0; i < FS_NODE_CAP; ++i)
        clear_bytes(&nodes[i], sizeof(nodes[i]));
    clear_bytes(bitmap, sizeof(bitmap));
    clear_bytes(legacy_metadata, sizeof(legacy_metadata));

    if (!storage_ready())
    {
        fs_say("FS: storage unavailable, using empty RAM fallback.\r\n");
        return -19;
    }

    int rc = load_super();
    if (rc == 0)
    {
        if (jnl_recover() != 0) fs_say("FS: journal recovery I/O error.\r\n");
        else if (jnl_replayed) { fs_say("FS: journal replayed, filesystem recovered.\r\n"); }
        rc = load_super();
    }
    if (rc == -2)
    {
        int blank = 1;
        if (storage_read_sector(0, io_buffer) == 0)
        {
            for (int i = 0; i < 512; ++i)
                if (io_buffer[i] != 0) { blank = 0; break; }
        }
        else blank = 0;
        rc = (blank && allow_format) ? format_fs() : -10;
    }
    if (rc != 0) return rc;

    if (mounted == 0)
    {
        if (load_all_inodes() != 0) { fs_say("FS: inode table read failed.\r\n"); return -11; }
        root_id = 0;

        /* Old images may keep data where the journal now lives: then run without it. */
        if (jnl_enabled)
            for (int i = 0; i < FS_MAX_NODES; ++i)
                if (nodes[i].used && nodes[i].sector_count &&
                    nodes[i].first_sector + nodes[i].sector_count > jnl_start())
                    jnl_enabled = 0;

        /* Rebuild the allocation bitmap from authoritative inode extents.
           This also repairs a stale/corrupt on-disk bitmap during mount. */
        if (rebuild_bitmap_from_inodes() != 0) { fs_say("FS: extent integrity check failed.\r\n"); return -12; }
        if (!validate_mounted_fs()) { fs_say("FS: inode integrity check failed.\r\n"); return -13; }
        fs_assign_legacy_owner_policy();
        if (write_bitmap() != 0) { fs_say("FS: bitmap repair failed.\r\n"); return -14; }
        for (int i = 0; i < FS_MAX_NODES; ++i)
        {
            if (nodes[i].used && legacy_metadata[i] && save_inode(i) != 0) { fs_say("FS: owner metadata migration failed.\r\n"); return -15; }
            legacy_metadata[i] = 0;
        }
        mounted = 1;
        fs_say("FS: filesystem mounted, verified and repaired.\r\n");
    }
    return 0;
}

void fs_init()
{
    uart_puts("FS: starting persistent mount...\r\n");
    for (int c = 0; c < FS_MAX_CTX; ++c) { clear_bytes(&ctxs[c], sizeof(ctxs[c])); ctxs[c].mp_ctx = -1; ctxs[c].mp_local = -1; }
    cur = &ctxs[0];
    cur->inuse = 1; cur->dev = 0;
    set_layout(FS_VERSION);
    if (mount_current(1) != 0)
    {
        uart_puts("FS: mount failed, RAM fallback enabled.\r\n");
        ram_fallback();
        return;
    }

    int storage_dir = ensure_dir_path("/storage");
    (void)storage_dir;
    int home_dir = ensure_dir_path("/storage/home");
    (void)home_dir;
    ensure_dir_path("/bin");
    ensure_dir_path("/home");
    ensure_dir_path("/home/user");
    int home_user = fs_resolve("/home/user", root_id);
    int storage_home = fs_resolve("/storage/home", root_id);
    if (home_user >= 0) (void)fs_set_owner(home_user, 1000U, 1000U);
    if (storage_home >= 0) (void)fs_set_owner(storage_home, 1000U, 1000U);

    /* Repair files that root created inside a user home before this rule
       existed (they would otherwise be read-only for the user). */
    for (int i = 0; i < FS_MAX_NODES; ++i)
    {
        if (!nodes[i].used || i == home_user || i == storage_home) continue;
        if (nodes[i].uid != 0U) continue; /* only root-created leftovers; keep explicit owners */
        if ((home_user >= 0 && fs_is_descendant_of(i, home_user)) ||
            (storage_home >= 0 && fs_is_descendant_of(i, storage_home)))
            (void)fs_set_owner(i, 1000U, 1000U);
    }
    ensure_dir_path("/etc");
    quota_load();
}

int fs_get_root() { return root_id; }

static int read_link_target(int id, char* out, int cap)
{
    if (id < 0 || id >= FS_MAX_NODES || !nodes[id].used || nodes[id].type != FS_SYMLINK) return -1;
    if (storage_ready() == 0 || nodes[id].size == 0 || nodes[id].size >= (uint64_t)cap || nodes[id].size > 255) return -1;
    static uint8_t lbuf[512] __attribute__((aligned(16)));
    if (storage_read_sector(nodes[id].first_sector, lbuf) != 0) return -1;
    for (uint64_t i = 0; i < nodes[id].size; ++i) out[i] = (char)lbuf[i];
    out[nodes[id].size] = '\0';
    return (int)nodes[id].size;
}

static int resolve_ex(const char* path, int cwd, int follow_last, int depth)
{
    if (path == 0) return -1;
    if (depth > 8) return -40;                 /* ELOOP */
    if (path[0] == '\0') return cwd;
    int current = path[0] == '/' ? root_id : cwd;
    int i = 0;
    while (path[i])
    {
        while (path[i] == '/') ++i;
        if (!path[i]) break;
        char part[FS_NAME_SIZE];
        int p = 0;
        while (path[i] && path[i] != '/')
        {
            if (p >= FS_NAME_SIZE - 1) return -1;
            part[p++] = path[i++];
        }
        part[p] = '\0';
        if (fs_check_access(current, FS_ACCESS_EXEC) != 0) return -13;
        if (str_equal(part, ".")) continue;
        if (str_equal(part, ".."))
        {
            if (current != root_id && current >= 0 && nodes[current].parent >= 0)
                current = nodes[current].parent;
            continue;
        }
        int parent_dir = current;
        current = find_child(current, part);
        if (current < 0) return -1;
        if (nodes[current].type == FS_SYMLINK)
        {
            int rest = 0;
            for (int k = i; path[k]; ++k) if (path[k] != '/') { rest = 1; break; }
            if (rest || follow_last)
            {
                char target[260];
                if (read_link_target(current, target, sizeof(target)) < 0) return -1;
                current = resolve_ex(target, parent_dir, 1, depth + 1);
                if (current < 0) return current;
            }
        }
    }
    return current;
}

int fs_resolve(const char* path, int cwd) { return resolve_ex(path, cwd, 1, 0); }
int fs_resolve_nofollow(const char* path, int cwd) { return resolve_ex(path, cwd, 0, 0); }

int fs_symlink(const char* target, const char* linkpath, int cwd)
{
    FS_RW_CHECK();
    if (!target || !target[0] || str_len(target) > 255 || !linkpath) return -22;
    FsTx fs_tx_scope;
    if (fs_resolve_nofollow(linkpath, cwd) >= 0) return -17;
    int id = create_path(linkpath, cwd, FS_SYMLINK);
    if (id < 0) return id;
    uint64_t first = 0;
    if (alloc_contiguous(1, &first) != 0) { clear_bytes(&nodes[id], sizeof(nodes[id])); (void)save_inode(id); return -3; }
    clear_bytes(io_buffer, FS_SECTOR_SIZE);
    int len = str_len(target);
    for (int i = 0; i < len; ++i) io_buffer[i] = (uint8_t)target[i];
    if (storage_write_sector(first, io_buffer) != 0)
    {
        free_sectors(first, 1);
        clear_bytes(&nodes[id], sizeof(nodes[id])); (void)save_inode(id); (void)write_bitmap();
        return -4;
    }
    nodes[id].size = (uint64_t)len; nodes[id].first_sector = first; nodes[id].sector_count = 1;
    nodes[id].mode = 0777U;
    if (save_inode(id) != 0 || write_bitmap() != 0) return -5;
    return id;
}

int fs_readlink(const char* path, int cwd, char* out, int cap)
{
    int id = fs_resolve_nofollow(path, cwd);
    if (id < 0) return id;
    if (nodes[id].type != FS_SYMLINK) return -22;
    return read_link_target(id, out, cap);
}

int fs_link_target_of(int id, char* out, int cap) { return read_link_target(id, out, cap); }

void fs_set_readonly(int ro) { fs_ro = ro ? 1 : 0; }
int fs_is_readonly() { return fs_ro; }
int fs_sync() { return storage_ready() ? storage_flush() : 0; }

void fs_journal_status(FsJournalInfo* out)
{
    if (!out) return;
    out->enabled = (uint32_t)jnl_enabled;
    out->start_sector = jnl_enabled ? jnl_start() : 0;
    out->sectors = jnl_enabled ? JNL_SECTORS : 0;
    out->commits = jnl_commits;
    out->replayed = jnl_replayed;
}

/* Test hook: the next transaction commits to the journal, then "loses power"
   before applying in place. The next mount must recover it. */
int fs_journal_crash_test(const char* path, int cwd)
{
    if (!jnl_enabled) return -1;
    jnl_crash_armed = 1;
    int rc = fs_write(path, cwd, "journal crash test: recovered after replay\n");
    jnl_crash_armed = 0;
    return rc;
}

int fs_mkdir(const char* path, int cwd) { FS_RW_CHECK(); FsTx fs_tx_scope; return create_path(path, cwd, FS_DIR); }

int fs_touch(const char* path, int cwd)
{
    FS_RW_CHECK();
    FsTx fs_tx_scope;
    int existing = fs_resolve(path, cwd);
    if (existing >= 0)
    {
        if (nodes[existing].type != FS_FILE) return -2;
        if (fs_check_access(existing, FS_ACCESS_WRITE) != 0) return -13;
        nodes[existing].mtime = rtc_now();
        return save_inode(existing) == 0 ? -2 : -4;
    }
    return create_path(path, cwd, FS_FILE);
}

int fs_mkdir_p(const char* path, int cwd)
{
    FS_RW_CHECK();
    FsTx fs_tx_scope;
    if (path == 0 || path[0] == '\0') return -1;
    char temp[192];
    if (str_len(path) >= (int)sizeof(temp)) return -1;
    str_copy(temp, path, sizeof(temp));

    int current = (temp[0] == '/') ? root_id : cwd;
    int i = (temp[0] == '/') ? 1 : 0;
    while (temp[i])
    {
        while (temp[i] == '/') ++i;
        if (!temp[i]) break;
        char part[FS_NAME_SIZE];
        int p = 0;
        while (temp[i] && temp[i] != '/')
        {
            if (p >= FS_NAME_SIZE - 1) return -1;
            part[p++] = temp[i++];
        }
        part[p] = '\0';
        /* Search permission applies even when the next component is . or ... */
        if (fs_check_access(current, FS_ACCESS_EXEC) != 0) return -13;
        if (str_equal(part, ".")) continue;
        if (str_equal(part, ".."))
        {
            if (current != root_id && nodes[current].parent >= 0) current = nodes[current].parent;
            continue;
        }
        int child = find_child(current, part);
        if (child >= 0)
        {
            if (nodes[child].type != FS_DIR) return -2;
            current = child;
            continue;
        }
        if (fs_check_access(current, FS_ACCESS_WRITE | FS_ACCESS_EXEC) != 0) return -13;
        int made = create_node(FS_DIR, part, current);
        if (made < 0) return made;
        current = made;
    }
    return current;
}


/* ---- hard links: FILE nodes whose (first_sector, sector_count, size) are identical share one extent ---- */
static int extent_shared_with_others(int self, uint64_t first, uint32_t count)
{
    if (count == 0) return 0;
    for (int j = 0; j < FS_MAX_NODES; ++j)
        if (j != self && nodes[j].used && nodes[j].type == FS_FILE && nodes[j].first_sector == first && nodes[j].sector_count == count)
            return 1;
    return 0;
}
static int link_count_of(int id)
{
    if (nodes[id].type != FS_FILE || nodes[id].sector_count == 0) return 1;
    int n = 1;
    for (int j = 0; j < FS_MAX_NODES; ++j)
        if (j != id && nodes[j].used && nodes[j].type == FS_FILE && nodes[j].first_sector == nodes[id].first_sector &&
            nodes[j].sector_count == nodes[id].sector_count && nodes[j].size == nodes[id].size) ++n;
    return n;
}
static void propagate_extent(int self, const FsNode* old)
{
    if (old->sector_count == 0) return;
    for (int j = 0; j < FS_MAX_NODES; ++j)
    {
        if (j == self || !nodes[j].used || nodes[j].type != FS_FILE) continue;
        if (nodes[j].first_sector != old->first_sector || nodes[j].sector_count != old->sector_count || nodes[j].size != old->size) continue;
        nodes[j].first_sector = nodes[self].first_sector;
        nodes[j].sector_count = nodes[self].sector_count;
        nodes[j].size = nodes[self].size;
        nodes[j].mtime = nodes[self].mtime;
        (void)save_inode(j);
    }
}

/* ---- per-user quota (bytes of allocated sectors); limits live in /etc/quota ("uid bytes" lines), 0 = none ---- */
#define QUOTA_MAX 16
static uint32_t quota_uid[QUOTA_MAX]; static uint64_t quota_bytes[QUOTA_MAX]; static int quota_n = 0;
static uint64_t quota_limit_for(uint32_t uid)
{
    for (int i = 0; i < quota_n; ++i) if (quota_uid[i] == uid) return quota_bytes[i];
    return 0;
}
static uint64_t usage_of_uid(uint32_t uid)
{
    uint64_t sectors = 0;
    for (int i = 0; i < FS_MAX_NODES; ++i)
    {
        if (!nodes[i].used || nodes[i].uid != uid || nodes[i].sector_count == 0) continue;
        int dup = 0;
        for (int j = 0; j < i; ++j) if (nodes[j].used && nodes[j].type == FS_FILE && nodes[i].type == FS_FILE && nodes[j].first_sector == nodes[i].first_sector && nodes[j].sector_count == nodes[i].sector_count) { dup = 1; break; }
        if (!dup) sectors += nodes[i].sector_count;
    }
    return sectors * FS_SECTOR_SIZE;
}

int fs_write_data(const char* path, int cwd, const uint8_t* data, uint64_t size)
{
    FS_RW_CHECK();
    FsTx fs_tx_scope;
    if (path == 0 || (data == 0 && size != 0) || size > FS_MAX_FILE_SIZE)
        return -1;

    int id = fs_resolve(path, cwd);
    int created = 0;
    if (id < 0)
    {
        /* Do not turn EACCES into ENOENT or try to create through a denied path. */
        if (id != -1) return id;
        id = fs_touch(path, cwd);
        if (id < 0) return id == -1 ? -2 : id;
        created = 1;
    }
    if (nodes[id].type != FS_FILE)
        return -2;
    if (fs_check_access(id, FS_ACCESS_WRITE) != 0) return -13;

    FsNode old; copy_node(&old, &nodes[id]);
    uint32_t need = (uint32_t)((size + FS_SECTOR_SIZE - 1ULL) / FS_SECTOR_SIZE);
    if (current_uid != 0U)
    {
        uint64_t lim = quota_limit_for(current_uid);
        if (lim)
        {
            uint64_t used = usage_of_uid(current_uid);
            uint64_t mine = (nodes[id].uid == current_uid) ? (uint64_t)old.sector_count * FS_SECTOR_SIZE : 0;
            if (used - (mine < used ? mine : used) + (uint64_t)need * FS_SECTOR_SIZE > lim)
            {
                if (created) { clear_bytes(&nodes[id], sizeof(nodes[id])); (void)save_inode(id); }
                return -122;                                   /* EDQUOT */
            }
        }
    }
    uint64_t new_first = 0;
    if (alloc_contiguous(need, &new_first) != 0)
        return -3;

    for (uint32_t s = 0; s < need; ++s)
    {
        clear_bytes(io_buffer, FS_SECTOR_SIZE);
        uint64_t offset = (uint64_t)s * FS_SECTOR_SIZE;
        uint64_t left = size > offset ? size - offset : 0;
        uint64_t take = left > FS_SECTOR_SIZE ? FS_SECTOR_SIZE : left;
        for (uint64_t i = 0; i < take; ++i)
            io_buffer[i] = data[offset + i];

        if (storage_write_sector(new_first + s, io_buffer) != 0)
        {
            free_sectors(new_first, need);
            if (created)
            {
                clear_bytes(&nodes[id], sizeof(nodes[id]));
                (void)save_inode(id);
            }
            (void)write_bitmap();
            return -4;
        }
    }

    nodes[id].size = size;
    nodes[id].first_sector = new_first;
    nodes[id].sector_count = need;
    nodes[id].mtime = rtc_now();

    /* Commit inode first while both old and new extents are still allocated. */
    if (save_inode(id) != 0)
    {
        copy_node(&nodes[id], &old);
        if (created)
        {
            clear_bytes(&nodes[id], sizeof(nodes[id]));
        }
        free_sectors(new_first, need);
        (void)save_inode(id);
        (void)write_bitmap();
        return -5;
    }

    propagate_extent(id, &old);                             /* hard links follow the new extent */

    /* Publish allocation before releasing the old extent. */
    if (write_bitmap() != 0)
    {
        /* The inode points to the new extent and it remains allocated; old blocks
           are intentionally retained rather than risking a dangling inode. */
        return -6;
    }

    free_sectors(old.first_sector, old.sector_count);
    if (write_bitmap() != 0)
        return -7;

    return storage_flush();
}

int fs_write(const char* path, int cwd, const char* content)
{
    if (content == 0) content = "";
    return fs_write_data(path, cwd, (const uint8_t*)content, (uint64_t)str_len(content));
}

static int fs_read_file_with_access(const char* path, int cwd, uint8_t* buffer,
                                    uint64_t capacity, uint64_t* out_size,
                                    uint32_t required_access)
{
    if (path == 0 || buffer == 0 || capacity == 0) return -1;
    int id = fs_resolve(path, cwd);
    if (id < 0) return id == -1 ? -2 : id;
    if (nodes[id].type != FS_FILE) return -2;
    if (fs_check_access(id, required_access) != 0) return -13;
    if (nodes[id].size > capacity) return -3;

    uint64_t remaining = nodes[id].size;
    for (uint32_t s = 0; s < nodes[id].sector_count; ++s)
    {
        if (storage_read_sector(nodes[id].first_sector + s, io_buffer) != 0) return -4;
        uint64_t take = remaining > FS_SECTOR_SIZE ? FS_SECTOR_SIZE : remaining;
        for (uint64_t i = 0; i < take; ++i)
            buffer[(uint64_t)s * FS_SECTOR_SIZE + i] = io_buffer[i];
        remaining -= take;
        if (remaining == 0) break;
    }
    if (remaining != 0) return -4;
    if (out_size) *out_size = nodes[id].size;
    return 0;
}

int fs_read_file(const char* path, int cwd, uint8_t* buffer, uint64_t capacity, uint64_t* out_size)
{
    return fs_read_file_with_access(path, cwd, buffer, capacity, out_size, FS_ACCESS_READ);
}

int fs_read_file_for_update(const char* path, int cwd, uint8_t* buffer,
                            uint64_t capacity, uint64_t* out_size)
{
    return fs_read_file_with_access(path, cwd, buffer, capacity, out_size, FS_ACCESS_WRITE);
}

int fs_cat(const char* path, int cwd)
{
    int id = fs_resolve(path, cwd);
    if (id < 0) return -1;
    if (nodes[id].type != FS_FILE) return -2;
    if (fs_check_access(id, FS_ACCESS_READ) != 0) return -13;
    uint64_t remaining = nodes[id].size;
    for (uint32_t s = 0; s < nodes[id].sector_count; ++s)
    {
        if (storage_read_sector(nodes[id].first_sector + s, io_buffer) != 0) return -3;
        uint64_t take = remaining > FS_SECTOR_SIZE ? FS_SECTOR_SIZE : remaining;
        for (uint64_t i = 0; i < take; ++i) uart_putc((char)io_buffer[i]);
        remaining -= take;
        if (remaining == 0) break;
    }
    return 0;
}

int fs_cat_numbered(const char* path, int cwd)
{
    int id = fs_resolve(path, cwd);
    if (id < 0) return -1;
    if (nodes[id].type != FS_FILE) return -2;
    if (fs_check_access(id, FS_ACCESS_READ) != 0) return -13;

    uint64_t remaining = nodes[id].size;
    uint64_t line = 1;
    int at_line_start = 1;
    for (uint32_t s = 0; s < nodes[id].sector_count; ++s)
    {
        if (storage_read_sector(nodes[id].first_sector + s, io_buffer) != 0) return -3;
        uint64_t take = remaining > FS_SECTOR_SIZE ? FS_SECTOR_SIZE : remaining;
        for (uint64_t i = 0; i < take; ++i)
        {
            char c = (char)io_buffer[i];
            if (at_line_start)
            {
                uint64_t value = line;
                char digits[24];
                int n = 0;
                do { digits[n++] = (char)('0' + (value % 10ULL)); value /= 10ULL; } while (value && n < (int)sizeof(digits));
                int pad = 6 - n;
                while (pad > 0) { uart_putc(' '); --pad; }
                while (n > 0) uart_putc(digits[--n]);
                uart_puts("  ");
                at_line_start = 0;
            }
            uart_putc(c);
            if (c == '\n')
            {
                ++line;
                at_line_start = 1;
            }
        }
        remaining -= take;
        if (remaining == 0) break;
    }
    return 0;
}

int fs_rm(const char* path, int cwd)
{
    FS_RW_CHECK();
    FsTx fs_tx_scope;
    int id = fs_resolve_nofollow(path, cwd);
    if (id < 0) return -1;
    if (id == root_id) return -3;
    if (nodes[id].mount_ctx > 0) return -16;                   /* a filesystem is mounted here */
    if (fs_parent_writable(id) != 0) return -13;
    if (nodes[id].type == FS_DIR)
    {
        for (int i = 0; i < FS_MAX_NODES; ++i)
            if (nodes[i].used && nodes[i].parent == id)
                return -2;
    }

    FsNode old; copy_node(&old, &nodes[id]);
    clear_bytes(&nodes[id], sizeof(nodes[id]));
    if (save_inode(id) != 0)
    {
        copy_node(&nodes[id], &old);
        (void)save_inode(id);
        return -4;
    }

    if (!(old.type == FS_FILE && extent_shared_with_others(id, old.first_sector, old.sector_count)))
        free_sectors(old.first_sector, old.sector_count);
    if (write_bitmap() != 0)
        return -5;

    return storage_flush();
}

static int rm_node_recursive(int id)
{
    if (id < 0 || id >= FS_MAX_NODES || !nodes[id].used) return -1;
    if (id == root_id) return -3;
    if (nodes[id].mount_ctx > 0) return -16;
    if (fs_parent_writable(id) != 0) return -13;
    if (nodes[id].type == FS_DIR && fs_check_access(id, FS_ACCESS_WRITE | FS_ACCESS_EXEC) != 0) return -13;

    /* Children are deleted before the parent. Re-query index 0 each time because
       node slots are compacted logically by the used flag. */
    while (1)
    {
        int child = -1;
        for (int i = 0; i < FS_MAX_NODES; ++i)
            if (nodes[i].used && nodes[i].parent == id) { child = i; break; }
        if (child < 0) break;
        int rc = rm_node_recursive(child);
        if (rc != 0) return rc;
    }

    FsNode old; copy_node(&old, &nodes[id]);
    clear_bytes(&nodes[id], sizeof(nodes[id]));
    if (save_inode(id) != 0) { copy_node(&nodes[id], &old); return -4; }
    if (!(old.type == FS_FILE && extent_shared_with_others(id, old.first_sector, old.sector_count)))
        free_sectors(old.first_sector, old.sector_count);
    if (write_bitmap() != 0) return -5;
    return storage_flush();
}

int fs_rm_recursive(const char* path, int cwd)
{
    FS_RW_CHECK();
    FsTx fs_tx_scope;
    int id = fs_resolve_nofollow(path, cwd);
    if (id < 0) return -1;
    if (id == root_id) return -3;
    return rm_node_recursive(id);
}

int fs_rename(const char* from, const char* to, int cwd)
{
    FS_RW_CHECK();
    FsTx fs_tx_scope;
    int id = fs_resolve_nofollow(from, cwd);
    if (id < 0) return -1;
    if (id == root_id) return -3;
    if (to == 0 || to[0] == '\0') return -5;

    int parent = -1;
    const char* base = 0;
    char temp[192];

    int dest = fs_resolve(to, cwd);
    if (dest >= 0)
    {
        if (nodes[dest].type != FS_DIR)
            return dest == id ? 0 : -2;
        parent = dest;                 /* move into an existing directory */
        base = nodes[id].name;
    }
    else
    {
        if (str_len(to) >= (int)sizeof(temp)) return -5;
        str_copy(temp, to, sizeof(temp));
        int len = str_len(temp);
        while (len > 1 && temp[len - 1] == '/')
            temp[--len] = '\0';

        int slash = -1;
        for (int i = 0; i < len; ++i)
            if (temp[i] == '/') slash = i;

        if (slash < 0)
        {
            parent = cwd;
            base = temp;
        }
        else if (slash == 0)
        {
            parent = root_id;
            base = temp + 1;
        }
        else
        {
            char parent_path[192];
            for (int i = 0; i < slash; ++i) parent_path[i] = temp[i];
            parent_path[slash] = '\0';
            parent = fs_resolve(parent_path, cwd);
            base = temp + slash + 1;
        }
    }

    if (parent < 0 || parent >= FS_MAX_NODES || !nodes[parent].used ||
        nodes[parent].type != FS_DIR)
        return -5;
    if (fs_parent_writable(id) != 0 ||
        fs_check_access(parent, FS_ACCESS_WRITE | FS_ACCESS_EXEC) != 0) return -13;
    if (base[0] == '\0' || str_equal(base, ".") || str_equal(base, "..") ||
        str_len(base) >= FS_NAME_SIZE)
        return -5;

    /* A directory may not be moved into itself or one of its descendants. */
    int hops = 0;
    for (int p = parent; p >= 0 && hops <= FS_MAX_NODES; p = nodes[p].parent, ++hops)
    {
        if (p == id) return -4;
        if (p == root_id) break;
    }

    int existing = find_child(parent, base);
    if (existing == id) return 0;      /* already there */
    if (existing >= 0) return -2;

    char new_name[FS_NAME_SIZE];
    str_copy(new_name, base, FS_NAME_SIZE);

    FsNode old; copy_node(&old, &nodes[id]);
    nodes[id].parent = parent;
    nodes[id].mtime = rtc_now();
    str_copy(nodes[id].name, new_name, FS_NAME_SIZE);
    if (save_inode(id) != 0)
    {
        copy_node(&nodes[id], &old);
        return -6;
    }
    return storage_flush() == 0 ? 0 : -6;
}

uint64_t fs_file_size(int id)
{
    if (id < 0 || id >= FS_MAX_NODES || !nodes[id].used) return 0;
    return nodes[id].size;
}
int fs_get_type(int id)
{
    if (id < 0 || id >= FS_MAX_NODES || !nodes[id].used) return -1;
    return nodes[id].type;
}
const char* fs_get_name(int id)
{
    if (id < 0 || id >= FS_MAX_NODES || !nodes[id].used) return "";
    return nodes[id].name;
}
int fs_get_parent(int id)
{
    if (id < 0 || id >= FS_MAX_NODES || !nodes[id].used) return -1;
    return nodes[id].parent;
}
int fs_get_child_count(int id)
{
    if (id < 0 || id >= FS_MAX_NODES || !nodes[id].used) return 0;
    int count = 0;
    for (int i = 0; i < FS_MAX_NODES; ++i)
        if (nodes[i].used && nodes[i].parent == id) ++count;
    return count;
}
int fs_get_child(int id, int index)
{
    if (id < 0 || id >= FS_MAX_NODES || !nodes[id].used || index < 0) return -1;
    int count = 0;
    for (int i = 0; i < FS_MAX_NODES; ++i)
    {
        if (nodes[i].used && nodes[i].parent == id)
        {
            if (count == index) return i;
            ++count;
        }
    }
    return -1;
}
uint32_t fs_get_mode(int id)
{
    if (id < 0 || id >= FS_MAX_NODES || !nodes[id].used) return 0;
    return nodes[id].mode;
}

uint64_t fs_get_mtime(int id)
{
    if (id < 0 || id >= FS_MAX_NODES || !nodes[id].used) return 0;
    return nodes[id].mtime;
}

uint32_t fs_get_uid(int id)
{
    if (id < 0 || id >= FS_MAX_NODES || !nodes[id].used) return 0xFFFFFFFFU;
    return nodes[id].uid;
}

uint32_t fs_get_gid(int id)
{
    if (id < 0 || id >= FS_MAX_NODES || !nodes[id].used) return 0xFFFFFFFFU;
    return nodes[id].gid;
}

int fs_set_mode(int id, uint32_t mode)
{
    FS_RW_CHECK();
    FsTx fs_tx_scope;
    if (id < 0 || id >= FS_MAX_NODES || !nodes[id].used) return -1;
    if (current_uid != 0 && current_uid != nodes[id].uid) return -13;
    if (mode & ~0777U) return -22;
    FsNode old; copy_node(&old, &nodes[id]);
    nodes[id].mode = mode;
    nodes[id].mtime = rtc_now();
    if (save_inode(id) != 0) { copy_node(&nodes[id], &old); return -4; }
    return storage_flush();
}

int fs_set_owner(int id, uint32_t uid, uint32_t gid)
{
    FS_RW_CHECK();
    FsTx fs_tx_scope;
    if (id < 0 || id >= FS_MAX_NODES || !nodes[id].used) return -1;
    if (current_uid != 0) return -13;
    if (uid > 0x7FFFFFFFU || gid > 0x7FFFFFFFU) return -22;
    FsNode old; copy_node(&old, &nodes[id]);
    nodes[id].uid = uid; nodes[id].gid = gid; nodes[id].mtime = rtc_now();
    if (save_inode(id) != 0) { copy_node(&nodes[id], &old); return -4; }
    return storage_flush();
}

int fs_set_owner_path(const char* path, int cwd, uint32_t uid, uint32_t gid)
{
    int id = fs_resolve(path, cwd);
    return id < 0 ? id : fs_set_owner(id, uid, gid);
}

int fs_stat_path(const char* path, int cwd, FsStat* out)
{
    if (!out) return -14;
    int id = fs_resolve(path, cwd);
    if (id < 0) return id;
    out->inode = (uint32_t)id;
    out->type = (uint32_t)nodes[id].type;
    out->mode = nodes[id].mode;
    out->uid = nodes[id].uid;
    out->gid = nodes[id].gid;
    out->sector_count = nodes[id].sector_count;
    out->size = nodes[id].size;
    out->mtime = nodes[id].mtime;
    return 0;
}

int fs_truncate(const char* path, int cwd, uint64_t size)
{
    if (size > FS_MAX_FILE_SIZE) return -27;
    int id = fs_resolve(path, cwd);
    if (id < 0) return id;
    if (nodes[id].type != FS_FILE) return -21;
    if (fs_check_access(id, FS_ACCESS_WRITE) != 0) return -13;

    uint64_t old_size = nodes[id].size;
    uint64_t remaining = old_size;
    for (uint32_t sector = 0; sector < nodes[id].sector_count; ++sector)
    {
        if (storage_read_sector(nodes[id].first_sector + sector, io_buffer) != 0) return -5;
        uint64_t take = remaining > FS_SECTOR_SIZE ? FS_SECTOR_SIZE : remaining;
        for (uint64_t i = 0; i < take; ++i)
            truncate_buffer[(uint64_t)sector * FS_SECTOR_SIZE + i] = io_buffer[i];
        remaining -= take;
        if (remaining == 0) break;
    }
    if (remaining != 0) return -5;
    if (size > old_size)
        for (uint64_t i = old_size; i < size; ++i) truncate_buffer[i] = 0;
    return fs_write_data(path, cwd, truncate_buffer, size);
}

uint64_t fs_total_sectors()
{
    return disk_sectors;
}

uint64_t fs_used_sectors()
{
    uint64_t used = 0;
    for (int i = 0; i < FS_MAX_NODES; ++i)
        if (nodes[i].used && nodes[i].type != FS_DIR)
            used += nodes[i].sector_count;
    return used;
}

int fs_set_mode_path(const char* path, int cwd, uint32_t mode)
{
    int id = fs_resolve(path, cwd);
    if (id < 0) return id;
    return fs_set_mode(id, mode);
}

int fs_persistent() { return mounted; }


/* hard link: linkpath becomes another name for the data of target (same extent) */
int fs_link_hard(const char* target, const char* linkpath, int cwd)
{
    FS_RW_CHECK();
    FsTx fs_tx_scope;
    int src = fs_resolve(target, cwd);
    if (src < 0) return -2;
    if (nodes[src].type != FS_FILE) return -21;                 /* directories and symlinks cannot be hard-linked */
    if (nodes[src].sector_count == 0) return -22;               /* empty files have no extent to share */
    if (fs_check_access(src, FS_ACCESS_READ) != 0) return -13;
    if (fs_resolve_nofollow(linkpath, cwd) >= 0) return -17;
    int id = create_path(linkpath, cwd, FS_FILE);
    if (id < 0) return id;
    nodes[id].size = nodes[src].size;
    nodes[id].first_sector = nodes[src].first_sector;
    nodes[id].sector_count = nodes[src].sector_count;
    nodes[id].mode = nodes[src].mode;
    nodes[id].uid = nodes[src].uid;
    nodes[id].gid = nodes[src].gid;
    nodes[id].mtime = nodes[src].mtime;
    if (save_inode(id) != 0) { clear_bytes(&nodes[id], sizeof(nodes[id])); (void)save_inode(id); return -5; }
    return id;
}
int fs_link_count(int id)
{
    if (id < 0 || id >= FS_MAX_NODES || !nodes[id].used) return 0;
    return link_count_of(id);
}

/* consistency check; repair rewrites the allocation bitmap from the inode table */
int fs_fsck(int repair, FsckInfo* out)
{
    if (!out) return -22;
    clear_bytes(out, sizeof(*out));
    if (repair) { FS_RW_CHECK(); }
    for (int i = 0; i < FS_MAX_NODES; ++i)
    {
        if (!nodes[i].used) continue;
        ++out->inodes;
        if (nodes[i].type == FS_DIR) ++out->dirs;
        else if (nodes[i].type == FS_SYMLINK) ++out->symlinks;
        else { ++out->files; if (link_count_of(i) > 1) ++out->hardlinks; }
    }
    out->structure_ok = validate_mounted_fs();
    static uint8_t saved[FS_MAX_BITMAP_BYTES];
    uint64_t nbytes = (disk_sectors + 7ULL) / 8ULL; if (nbytes > sizeof(saved)) nbytes = sizeof(saved);
    for (uint64_t i = 0; i < nbytes; ++i) saved[i] = bitmap[i];
    if (rebuild_bitmap_from_inodes() != 0)
    {
        for (uint64_t i = 0; i < nbytes; ++i) bitmap[i] = saved[i];
        out->bitmap_ok = 0; out->structure_ok = 0;
        return 0;
    }
    for (uint64_t sct = 0; sct < disk_sectors; ++sct)
    {
        int a = (saved[sct >> 3] >> (sct & 7)) & 1, b = bitmap_get(sct);
        if (a && !b) ++out->leaked;
        else if (!a && b) ++out->missing;
        if (b) ++out->sectors_used;
    }
    out->bitmap_ok = out->leaked == 0 && out->missing == 0;
    if (repair && !out->bitmap_ok)
    {
        FsTx fs_tx_scope;
        if (write_bitmap() != 0) return -5;
        out->repaired = 1;
    }
    else for (uint64_t i = 0; i < nbytes; ++i) bitmap[i] = saved[i];
    return 0;
}

int fs_quota_set(uint32_t uid, uint64_t bytes)
{
    FS_RW_CHECK();
    int slot = -1;
    for (int i = 0; i < quota_n; ++i) if (quota_uid[i] == uid) slot = i;
    if (slot < 0) { if (quota_n >= QUOTA_MAX) return -24; slot = quota_n++; quota_uid[slot] = uid; }
    quota_bytes[slot] = bytes;
    if (bytes == 0) { quota_uid[slot] = quota_uid[quota_n - 1]; quota_bytes[slot] = quota_bytes[quota_n - 1]; --quota_n; }
    static char b[QUOTA_MAX * 32]; int p = 0;
    for (int i = 0; i < quota_n; ++i)
    {
        char t[24]; int n = 0; uint64_t v = quota_uid[i]; do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
        while (n) b[p++] = t[--n];
        b[p++] = ' ';
        v = quota_bytes[i]; do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
        while (n) b[p++] = t[--n];
        b[p++] = '\n';
    }
    uint32_t saved_uid = current_uid; current_uid = 0;
    int rc = fs_write_data("/etc/quota", root_id, (const uint8_t*)b, (uint64_t)p);
    current_uid = saved_uid;
    return rc < 0 ? rc : 0;
}
int fs_quota_get(uint32_t uid, uint64_t* limit, uint64_t* used)
{
    if (limit) *limit = quota_limit_for(uid);
    if (used) *used = usage_of_uid(uid);
    return 0;
}
static void quota_load()
{
    static uint8_t b[QUOTA_MAX * 32 + 8]; uint64_t sz = 0; quota_n = 0;
    if (fs_read_file("/etc/quota", root_id, b, sizeof(b) - 1, &sz) != 0) return;
    uint64_t i = 0;
    while (i < sz && quota_n < QUOTA_MAX)
    {
        uint64_t u = 0, v = 0;
        while (i < sz && b[i] >= '0' && b[i] <= '9') u = u * 10 + (b[i++] - '0');
        if (i < sz && b[i] == ' ') ++i;
        while (i < sz && b[i] >= '0' && b[i] <= '9') v = v * 10 + (b[i++] - '0');
        while (i < sz && b[i] != '\n') ++i;
        ++i;
        if (v) { quota_uid[quota_n] = (uint32_t)u; quota_bytes[quota_n] = v; ++quota_n; }
    }
}

/* ============================================================================================
   Public API with multi-mount support.
   Global node id = (mount index << 12) | local inode index (mount 0 = root, so root ids are unchanged).
   Path functions first walk the path to the mount that holds it (crossing mount points and '..'
   back out of them), then call the single-mount implementation on that mount.
   ============================================================================================ */
#undef fs_check_access_as
#undef fs_check_access
#undef fs_init
#undef fs_get_root
#undef fs_resolve
#undef fs_resolve_nofollow
#undef fs_symlink
#undef fs_readlink
#undef fs_link_target_of
#undef fs_set_readonly
#undef fs_is_readonly
#undef fs_sync
#undef fs_journal_status
#undef fs_journal_crash_test
#undef fs_mkdir
#undef fs_touch
#undef fs_mkdir_p
#undef fs_write_data
#undef fs_write
#undef fs_read_file
#undef fs_read_file_for_update
#undef fs_cat
#undef fs_cat_numbered
#undef fs_rm
#undef fs_rm_recursive
#undef fs_rename
#undef fs_file_size
#undef fs_get_type
#undef fs_get_name
#undef fs_get_parent
#undef fs_get_child_count
#undef fs_get_child
#undef fs_get_mode
#undef fs_get_mtime
#undef fs_get_uid
#undef fs_get_gid
#undef fs_set_mode
#undef fs_set_owner
#undef fs_set_owner_path
#undef fs_stat_path
#undef fs_truncate
#undef fs_total_sectors
#undef fs_used_sectors
#undef fs_set_mode_path
#undef fs_persistent
#undef fs_link_hard
#undef fs_link_count
#undef fs_fsck
#undef fs_quota_set
#undef fs_quota_get

#undef nodes
#undef bitmap
#undef legacy_metadata
#undef disk_sectors
#undef root_id
#undef mounted
#undef node_count
#undef g_inode_sectors
#undef g_bitmap_start
#undef g_data_start
#undef g_version
#undef jnl_enabled
#undef fs_ro
#undef jnl_seq
static int enc(int c, int l) { return l < 0 ? l : ((c << 12) | l); }
static int gid_ok(int g) { if (g < 0) return 0; int c = g >> 12; return c < FS_MAX_CTX && ctxs[c].inuse; }
static void act(int c) { cur = &ctxs[c]; }

struct Route { int ctx; int cwd; const char* rest; char buf[200]; };

static void route(const char* path, int cwd_gid, Route* r, int cross_last)
{
    int c = 0, loc = cwd_gid;
    if (cwd_gid >= 0) { if (gid_ok(cwd_gid)) { c = cwd_gid >> 12; loc = cwd_gid & 0xFFF; } else { c = 0; loc = -1; } }
    r->rest = path;
    if (!path) { act(c); r->ctx = c; r->cwd = loc; return; }
    if (path[0] == '/') { c = 0; loc = 0; }
    act(c);
    r->ctx = c; r->cwd = loc;
    if (loc < 0) return;
    if (str_len(path) >= (int)sizeof(r->buf)) return;                 /* too long: leave it to the implementation */
    char* b = r->buf;
    str_copy(b, path, sizeof(r->buf));
    int n = str_len(b);
    while (n > 0 && b[n - 1] == '/') b[--n] = 0;
    int i = 0;
    for (;;)
    {
        while (b[i] == '/') ++i;
        if (!b[i]) { r->rest = b + i; break; }
        int st = i;
        while (b[i] && b[i] != '/') ++i;
        int ce = i;
        int k = i; while (b[k] == '/') ++k;
        int more = b[k] != 0;
        char save = b[ce]; b[ce] = 0;
        const char* comp = b + st;
        int is_dot = comp[0] == '.' && comp[1] == 0;
        int is_dd = comp[0] == '.' && comp[1] == '.' && comp[2] == 0;
        int stop = 0;
        if (is_dot) { }
        else if (is_dd)
        {
            if (fs_check_access_impl(loc, FS_ACCESS_EXEC) != 0) stop = 1;
            else if (loc == 0)
            {
                if (c > 0)
                {
                    int pc = ctxs[c].mp_ctx, pl = ctxs[c].mp_local;
                    c = pc; act(c); loc = pl;
                    if (loc != 0 && cur->nodes[loc].parent >= 0) loc = cur->nodes[loc].parent;
                }
            }
            else if (cur->nodes[loc].parent >= 0) loc = cur->nodes[loc].parent;
        }
        else
        {
            if (fs_check_access_impl(loc, FS_ACCESS_EXEC) != 0) stop = 1;
            else
            {
                int ch = find_child(loc, comp);
                if (ch < 0) stop = 1;
                else if (cur->nodes[ch].mount_ctx > 0 && (more || cross_last)) { c = cur->nodes[ch].mount_ctx; act(c); loc = 0; }
                else if (!more) stop = 1;
                else if (cur->nodes[ch].type != FS_DIR) stop = 1;
                else loc = ch;
            }
        }
        b[ce] = save;
        if (stop) { r->rest = b + st; break; }
    }
    r->ctx = c; r->cwd = loc;
}

/* path of directory `loc` inside mount c, from that mount's root (no leading mount prefix) */
static void ctx_rel_path(int c, int loc, char* out, int cap)
{
    act(c);
    char parts[12][FS_NAME_SIZE]; int n = 0;
    while (loc > 0 && n < 12 && cur->nodes[loc].used) { str_copy(parts[n++], cur->nodes[loc].name, FS_NAME_SIZE); loc = cur->nodes[loc].parent; }
    int p = 0;
    out[p++] = '/';
    for (int i = n - 1; i >= 0 && p < cap - 2; --i)
    {
        for (const char* q = parts[i]; *q && p < cap - 2; ++q) out[p++] = *q;
        if (i > 0) out[p++] = '/';
    }
    out[p] = 0;
}

int fs_check_access_as(int id, uint32_t uid, uint32_t gid, uint32_t requested)
{ if (!gid_ok(id)) return -2; act(id >> 12); return fs_check_access_as_impl(id & 0xFFF, uid, gid, requested); }
int fs_check_access(int id, uint32_t requested) { return fs_check_access_as(id, current_uid, current_gid, requested); }
int fs_get_root() { return 0; }
int fs_resolve(const char* path, int cwd) { Route r; route(path, cwd, &r, 1); int v = fs_resolve_impl(r.rest, r.cwd); return enc(r.ctx, v); }
int fs_resolve_nofollow(const char* path, int cwd) { Route r; route(path, cwd, &r, 1); int v = fs_resolve_nofollow_impl(r.rest, r.cwd); return enc(r.ctx, v); }
int fs_symlink(const char* target, const char* linkpath, int cwd) { Route r; route(linkpath, cwd, &r, 0); int v = fs_symlink_impl(target, r.rest, r.cwd); return enc(r.ctx, v); }
int fs_readlink(const char* path, int cwd, char* out, int cap) { Route r; route(path, cwd, &r, 1); return fs_readlink_impl(r.rest, r.cwd, out, cap); }
int fs_link_target_of(int id, char* out, int cap) { if (!gid_ok(id)) return -1; act(id >> 12); return fs_link_target_of_impl(id & 0xFFF, out, cap); }
void fs_set_readonly(int ro) { act(0); fs_set_readonly_impl(ro); }
int fs_is_readonly() { act(0); return fs_is_readonly_impl(); }
int fs_sync()
{
    int rc = 0;
    for (int c = 0; c < FS_MAX_CTX; ++c) if (ctxs[c].inuse) { act(c); int r = fs_sync_impl(); if (r != 0) rc = r; }
    act(0);
    return rc;
}
void fs_journal_status(FsJournalInfo* out) { act(0); fs_journal_status_impl(out); }
int fs_journal_crash_test(const char* path, int cwd) { Route r; route(path, cwd, &r, 0); return fs_journal_crash_test_impl(r.rest, r.cwd); }
int fs_mkdir(const char* path, int cwd) { Route r; route(path, cwd, &r, 0); int v = fs_mkdir_impl(r.rest, r.cwd); return enc(r.ctx, v); }
int fs_touch(const char* path, int cwd) { Route r; route(path, cwd, &r, 0); int v = fs_touch_impl(r.rest, r.cwd); return enc(r.ctx, v); }
int fs_mkdir_p(const char* path, int cwd) { Route r; route(path, cwd, &r, 0); int v = fs_mkdir_p_impl(r.rest, r.cwd); return enc(r.ctx, v); }
int fs_write_data(const char* path, int cwd, const uint8_t* data, uint64_t size) { Route r; route(path, cwd, &r, 0); return fs_write_data_impl(r.rest, r.cwd, data, size); }
int fs_write(const char* path, int cwd, const char* content) { Route r; route(path, cwd, &r, 0); return fs_write_impl(r.rest, r.cwd, content); }
int fs_read_file(const char* path, int cwd, uint8_t* buffer, uint64_t capacity, uint64_t* out_size) { Route r; route(path, cwd, &r, 1); return fs_read_file_impl(r.rest, r.cwd, buffer, capacity, out_size); }
int fs_read_file_for_update(const char* path, int cwd, uint8_t* buffer, uint64_t capacity, uint64_t* out_size) { Route r; route(path, cwd, &r, 1); return fs_read_file_for_update_impl(r.rest, r.cwd, buffer, capacity, out_size); }
int fs_cat(const char* path, int cwd) { Route r; route(path, cwd, &r, 1); return fs_cat_impl(r.rest, r.cwd); }
int fs_cat_numbered(const char* path, int cwd) { Route r; route(path, cwd, &r, 1); return fs_cat_numbered_impl(r.rest, r.cwd); }
int fs_rm(const char* path, int cwd) { Route r; route(path, cwd, &r, 1); return fs_rm_impl(r.rest, r.cwd); }
int fs_rm_recursive(const char* path, int cwd) { Route r; route(path, cwd, &r, 1); return fs_rm_recursive_impl(r.rest, r.cwd); }
int fs_rename(const char* from, const char* to, int cwd)
{
    Route a, b;
    route(from, cwd, &a, 1);
    route(to, cwd, &b, 0);
    if (a.ctx != b.ctx) return -18;                              /* EXDEV: different filesystems (cp + rm instead) */
    if (!a.rest || !b.rest) { act(a.ctx); return fs_rename_impl(a.rest, b.rest, a.cwd); }
    char fa[200], fb[200], da[200], db[200];
    ctx_rel_path(a.ctx, a.cwd, da, sizeof(da));
    ctx_rel_path(b.ctx, b.cwd, db, sizeof(db));
    int p = 0; for (const char* q = da; *q && p < 150; ++q) fa[p++] = *q;
    if (p > 1 && a.rest[0]) fa[p++] = '/';
    for (const char* q = a.rest; *q && p < 198; ++q) fa[p++] = *q;
    fa[p] = 0;
    p = 0; for (const char* q = db; *q && p < 150; ++q) fb[p++] = *q;
    if (p > 1 && b.rest[0]) fb[p++] = '/';
    for (const char* q = b.rest; *q && p < 198; ++q) fb[p++] = *q;
    fb[p] = 0;
    act(a.ctx);
    return fs_rename_impl(fa, fb, 0);
}
uint64_t fs_file_size(int id) { if (!gid_ok(id)) return 0; act(id >> 12); return fs_file_size_impl(id & 0xFFF); }
int fs_get_type(int id) { if (!gid_ok(id)) return -1; act(id >> 12); return fs_get_type_impl(id & 0xFFF); }
const char* fs_get_name(int id)
{
    if (!gid_ok(id)) return "";
    int c = id >> 12;
    if ((id & 0xFFF) == 0 && c > 0) { act(ctxs[c].mp_ctx); return fs_get_name_impl(ctxs[c].mp_local); }
    act(c); return fs_get_name_impl(id & 0xFFF);
}
int fs_get_parent(int id)
{
    if (!gid_ok(id)) return -1;
    int c = id >> 12;
    if ((id & 0xFFF) == 0 && c > 0) return enc(ctxs[c].mp_ctx, ctxs[c].mp_local);
    act(c); return enc(c, fs_get_parent_impl(id & 0xFFF));
}
int fs_get_child_count(int id) { if (!gid_ok(id)) return 0; act(id >> 12); return fs_get_child_count_impl(id & 0xFFF); }
int fs_get_child(int id, int index) { if (!gid_ok(id)) return -1; act(id >> 12); return enc(id >> 12, fs_get_child_impl(id & 0xFFF, index)); }
uint32_t fs_get_mode(int id) { if (!gid_ok(id)) return 0; act(id >> 12); return fs_get_mode_impl(id & 0xFFF); }
uint64_t fs_get_mtime(int id) { if (!gid_ok(id)) return 0; act(id >> 12); return fs_get_mtime_impl(id & 0xFFF); }
uint32_t fs_get_uid(int id) { if (!gid_ok(id)) return 0; act(id >> 12); return fs_get_uid_impl(id & 0xFFF); }
uint32_t fs_get_gid(int id) { if (!gid_ok(id)) return 0; act(id >> 12); return fs_get_gid_impl(id & 0xFFF); }
int fs_set_mode(int id, uint32_t mode) { if (!gid_ok(id)) return -1; act(id >> 12); return fs_set_mode_impl(id & 0xFFF, mode); }
int fs_set_owner(int id, uint32_t uid, uint32_t gid) { if (!gid_ok(id)) return -1; act(id >> 12); return fs_set_owner_impl(id & 0xFFF, uid, gid); }
int fs_set_owner_path(const char* path, int cwd, uint32_t uid, uint32_t gid) { Route r; route(path, cwd, &r, 1); return fs_set_owner_path_impl(r.rest, r.cwd, uid, gid); }
int fs_stat_path(const char* path, int cwd, FsStat* out) { Route r; route(path, cwd, &r, 1); return fs_stat_path_impl(r.rest, r.cwd, out); }
int fs_truncate(const char* path, int cwd, uint64_t size) { Route r; route(path, cwd, &r, 0); return fs_truncate_impl(r.rest, r.cwd, size); }
uint64_t fs_total_sectors() { act(0); return fs_total_sectors_impl(); }
uint64_t fs_used_sectors() { act(0); return fs_used_sectors_impl(); }
int fs_set_mode_path(const char* path, int cwd, uint32_t mode) { Route r; route(path, cwd, &r, 1); return fs_set_mode_path_impl(r.rest, r.cwd, mode); }
int fs_persistent() { return ctxs[0].mounted; }
void fs_init() { fs_init_impl(); act(0); }

int fs_link_hard(const char* target, const char* linkpath, int cwd)
{
    Route a, b; route(target, cwd, &a, 1); route(linkpath, cwd, &b, 0);
    if (a.ctx != b.ctx) return -18;
    if (!a.rest || !b.rest) return -22;
    char fa[200], fb[200], da[200], db[200];
    ctx_rel_path(a.ctx, a.cwd, da, sizeof(da)); ctx_rel_path(b.ctx, b.cwd, db, sizeof(db));
    int p = 0; for (const char* q = da; *q && p < 150; ++q) fa[p++] = *q;
    if (p > 1 && a.rest[0]) fa[p++] = '/';
    for (const char* q = a.rest; *q && p < 198; ++q) fa[p++] = *q;
    fa[p] = 0;
    p = 0; for (const char* q = db; *q && p < 150; ++q) fb[p++] = *q;
    if (p > 1 && b.rest[0]) fb[p++] = '/';
    for (const char* q = b.rest; *q && p < 198; ++q) fb[p++] = *q;
    fb[p] = 0;
    act(a.ctx);
    int v = fs_link_hard_impl(fa, fb, 0);
    return enc(a.ctx, v);
}
int fs_link_count(int id) { if (!gid_ok(id)) return 0; act(id >> 12); return fs_link_count_impl(id & 0xFFF); }
int fs_fsck(const char* path, int cwd, int repair, FsckInfo* out)
{
    int c = 0;
    if (path && path[0]) { Route r; route(path, cwd, &r, 1); if (r.cwd != 0 || (r.rest && r.rest[0])) { act(0); return -22; } c = r.ctx; }
    act(c); int rc = fs_fsck_impl(repair, out); act(0); return rc;
}
int fs_quota_set(uint32_t uid, uint64_t bytes) { act(0); return fs_quota_set_impl(uid, bytes); }
int fs_quota_get(uint32_t uid, uint64_t* limit, uint64_t* used) { act(0); return fs_quota_get_impl(uid, limit, used); }

/* ---- mount management ---- */
static void gid_path(int gid, char* out, int cap)
{
    char parts[12][FS_NAME_SIZE]; int n = 0;
    while (gid >= 0 && gid != 0 && n < 12) { str_copy(parts[n++], fs_get_name(gid), FS_NAME_SIZE); gid = fs_get_parent(gid); }
    int p = 0; out[p++] = '/';
    for (int i = n - 1; i >= 0 && p < cap - 2; --i) { for (const char* q = parts[i]; *q && p < cap - 2; ++q) out[p++] = *q; if (i > 0) out[p++] = '/'; }
    out[p] = 0;
}

static int dev_in_use(int dev) { for (int c = 0; c < FS_MAX_CTX; ++c) if (ctxs[c].inuse && ctxs[c].dev == dev) return 1; return 0; }

int fs_mount_count() { int n = 0; for (int c = 0; c < FS_MAX_CTX; ++c) if (ctxs[c].inuse) ++n; return n; }

int fs_mount_info(int index, FsMountInfo* out)
{
    if (!out) return -22;
    int n = 0;
    for (int c = 0; c < FS_MAX_CTX; ++c)
    {
        if (!ctxs[c].inuse) continue;
        if (n++ != index) continue;
        out->dev = ctxs[c].dev; out->readonly = ctxs[c].fs_ro; out->journal = ctxs[c].jnl_enabled; out->version = (int)ctxs[c].g_version;
        out->total_sectors = ctxs[c].disk_sectors;
        act(c); out->used_sectors = fs_used_sectors_impl(); act(0);
        if (c == 0) { out->dir[0] = '/'; out->dir[1] = 0; }
        else gid_path(enc(ctxs[c].mp_ctx, ctxs[c].mp_local), out->dir, sizeof(out->dir));
        return 0;
    }
    return -1;
}

int fs_mount_device(int dev, const char* path, int cwd, int readonly, int format_fresh)
{
    if (dev < 0 || !path) return -22;
    if (dev_in_use(dev)) return -16;
    if (!storage_dev_sectors) return -19;
    uint64_t cap = storage_dev_sectors(dev);
    if (cap == 0) return -19;
    Route r; route(path, cwd, &r, 1);
    int pc = r.ctx, node;
    act(pc);
    if (!r.rest || !r.rest[0]) node = r.cwd;
    else node = find_child(r.cwd, r.rest);
    if (node < 0 || !cur->nodes[node].used) return -2;
    if (cur->nodes[node].type != FS_DIR) return -20;
    if (cur->nodes[node].mount_ctx > 0 || (node == 0 && pc > 0)) return -16;
    int slot = -1;
    for (int c = 1; c < FS_MAX_CTX; ++c) if (!ctxs[c].inuse) { slot = c; break; }
    if (slot < 0) return -24;
    clear_bytes(&ctxs[slot], sizeof(ctxs[slot]));
    ctxs[slot].inuse = 1; ctxs[slot].dev = dev; ctxs[slot].fs_ro = readonly ? 1 : 0;
    ctxs[slot].mp_ctx = pc; ctxs[slot].mp_local = node;
    ctxs[slot].quiet = (dev == 1 || dev == 5);
    ctxs[slot].no_journal = (dev == 1 || dev == 5);
    ctxs[slot].fmt_sectors = cap > FS_DEFAULT_DISK_SECTORS ? FS_DEFAULT_DISK_SECTORS : cap;
    act(slot);
    set_layout(FS_VERSION);
    int rc;
    if (format_fresh)
    {
        int saved_ro = cur->fs_ro; cur->fs_ro = 0;
        rc = format_fs();
        if (rc == 0 && ctxs[slot].quiet) { cur->nodes[0].mode = 0777U; (void)save_inode(0); }
        cur->fs_ro = saved_ro;
    }
    else rc = mount_current(0);
    if (rc != 0 || !cur->mounted)
    {
        clear_bytes(&ctxs[slot], sizeof(ctxs[slot]));
        act(0);
        return rc != 0 ? rc : -10;
    }
    act(pc);
    cur->nodes[node].mount_ctx = slot;
    act(0);
    return 0;
}

int fs_mkfs_device(int dev)
{
    if (dev <= 0 || !storage_dev_sectors) return -22;
    if (dev_in_use(dev)) return -16;
    uint64_t cap = storage_dev_sectors(dev);
    if (cap == 0) return -19;
    int slot = -1;
    for (int c = 1; c < FS_MAX_CTX; ++c) if (!ctxs[c].inuse) { slot = c; break; }
    if (slot < 0) return -24;
    clear_bytes(&ctxs[slot], sizeof(ctxs[slot]));
    ctxs[slot].inuse = 1; ctxs[slot].dev = dev; ctxs[slot].quiet = 1;
    ctxs[slot].no_journal = (dev == 1 || dev == 5);
    ctxs[slot].fmt_sectors = cap > FS_DEFAULT_DISK_SECTORS ? FS_DEFAULT_DISK_SECTORS : cap;
    ctxs[slot].mp_ctx = -1; ctxs[slot].mp_local = -1;
    act(slot);
    set_layout(FS_VERSION);
    int rc = format_fs();
    if (rc == 0) (void)storage_flush();
    clear_bytes(&ctxs[slot], sizeof(ctxs[slot]));
    act(0);
    return rc;
}

/* unmount the filesystem cur->mounted at `path` (the path must be the mount's root directory) */
int fs_umount_path(const char* path, int cwd, int busy_gid)
{
    Route r; route(path, cwd, &r, 1);
    int c = r.ctx;
    if (c == 0 || r.cwd != 0 || (r.rest && r.rest[0])) { act(0); return -22; }
    if (busy_gid >= 0 && (busy_gid >> 12) == c) { act(0); return -16; }
    for (int k = 1; k < FS_MAX_CTX; ++k) if (ctxs[k].inuse && ctxs[k].mp_ctx == c) { act(0); return -16; }
    act(c); (void)fs_sync_impl();
    int pc = ctxs[c].mp_ctx, pl = ctxs[c].mp_local;
    act(pc); cur->nodes[pl].mount_ctx = 0;
    clear_bytes(&ctxs[c], sizeof(ctxs[c]));
    act(0);
    return 0;
}
