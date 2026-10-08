#include "fs.h"
#include "storage.h"
#include "uart.h"
#include "string.h"
#include "rtc.h"

static uint64_t rtc_now()
{
    return rtc_get_epoch();
}

#define FS_MAGIC            0x4D4B4653U
#define FS_VERSION          1U
#define FS_SECTOR_SIZE      512ULL
#define FS_MAX_NODES        64
#define FS_NAME_SIZE        64
#define FS_INODE_SIZE       128
#define FS_INODE_SECTORS    16
#define FS_BITMAP_SECTORS   8
#define FS_INODE_START      1ULL
#define FS_BITMAP_START     (FS_INODE_START + FS_INODE_SECTORS)
#define FS_DATA_START       (FS_BITMAP_START + FS_BITMAP_SECTORS)
#define FS_MAX_FILE_SIZE    (256ULL * 1024ULL)
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
    uint64_t mtime;
    uint64_t size;
    uint64_t first_sector;
    uint32_t sector_count;
    char name[FS_NAME_SIZE];
};

static FsNode nodes[FS_MAX_NODES];
static uint8_t bitmap[FS_MAX_BITMAP_BYTES];
static uint64_t disk_sectors = 0;
static int root_id = -1;
static int mounted = 0;
static uint8_t io_buffer[FS_SECTOR_SIZE] __attribute__((aligned(16)));
static void clear_bytes(void* ptr, uint64_t size);
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
        if (storage_write_sector(FS_BITMAP_START + s, src) != 0)
            return -1;
    }
    return storage_flush();
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
    str_copy(n->name, d->name, FS_NAME_SIZE);
}

static int save_inode(int id)
{
    if (id < 0 || id >= FS_MAX_NODES)
        return -1;
    uint64_t index = (uint64_t)id;
    uint64_t sector = FS_INODE_START + (index * FS_INODE_SIZE) / FS_SECTOR_SIZE;
    uint64_t offset = (index * FS_INODE_SIZE) % FS_SECTOR_SIZE;
    if (storage_read_sector(sector, io_buffer) != 0)
        return -1;
    DiskInode d;
    node_to_disk(&nodes[id], &d);
    for (uint64_t i = 0; i < sizeof(d); ++i)
        io_buffer[offset + i] = ((const uint8_t*)&d)[i];
    if (storage_write_sector(sector, io_buffer) != 0)
        return -1;
    return storage_flush();
}

static int save_super()
{
    DiskSuper sb;
    clear_bytes(&sb, sizeof(sb));
    sb.magic = FS_MAGIC;
    sb.version = FS_VERSION;
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
    if (storage_write_sector(0, io_buffer) != 0)
        return -1;
    return storage_flush();
}

static int load_super()
{
    if (storage_read_sector(0, io_buffer) != 0)
        return -1;
    DiskSuper sb;
    for (uint64_t i = 0; i < sizeof(sb); ++i)
        ((uint8_t*)&sb)[i] = io_buffer[i];
    if (sb.magic != FS_MAGIC || sb.version != FS_VERSION ||
        sb.sector_size != FS_SECTOR_SIZE || sb.inode_size != FS_INODE_SIZE ||
        sb.inode_count != FS_MAX_NODES || sb.data_start != FS_DATA_START)
        return -2;
    if (sb.total_sectors < FS_DATA_START || sb.total_sectors > FS_MAX_DISK_SECTORS)
        return -3;
    disk_sectors = sb.total_sectors;
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
            disk_to_node(&d, &nodes[(int)(s * 4ULL + slot)]);
            if (nodes[(int)(s * 4ULL + slot)].used)
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
        if (!nodes[i].used || nodes[i].type != FS_FILE || nodes[i].sector_count == 0)
            continue;

        if (nodes[i].first_sector < FS_DATA_START ||
            nodes[i].sector_count > disk_sectors - nodes[i].first_sector)
            return -1;

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
            if (nodes[i].name[0] == '\0' || str_len(nodes[i].name) >= FS_NAME_SIZE)
                return 0;
        }

        if (nodes[i].type == FS_FILE)
        {
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
                    nodes[i].sector_count > disk_sectors - nodes[i].first_sector)
                    return 0;
                for (uint32_t s = 0; s < nodes[i].sector_count; ++s)
                    if (!bitmap_get(nodes[i].first_sector + s))
                        return 0;
            }
        }
        else if (nodes[i].type != FS_DIR)
        {
            return 0;
        }
    }

    /* Detect duplicated/overlapping allocated extents between files. */
    for (int a = 0; a < FS_MAX_NODES; ++a)
    {
        if (!nodes[a].used || nodes[a].type != FS_FILE || nodes[a].sector_count == 0)
            continue;
        uint64_t a0 = nodes[a].first_sector;
        uint64_t a1 = a0 + nodes[a].sector_count;
        for (int b = a + 1; b < FS_MAX_NODES; ++b)
        {
            if (!nodes[b].used || nodes[b].type != FS_FILE || nodes[b].sector_count == 0)
                continue;
            uint64_t b0 = nodes[b].first_sector;
            uint64_t b1 = b0 + nodes[b].sector_count;
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
}

static int format_fs()
{
    disk_sectors = FS_DEFAULT_DISK_SECTORS;
    uart_puts("FS: formatting persistent filesystem...\r\n");
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
    nodes[0].mtime = rtc_now();
    str_copy(nodes[0].name, "/", FS_NAME_SIZE);
    if (save_inode(0) != 0)
        return -1;

    for (int i = 1; i < FS_MAX_NODES; ++i)
        nodes[i].used = 0;

    mounted = 1;
    uart_puts("FS: persistent filesystem ready.\r\n");
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
    return create_node(type, name, parent);
}

static int ensure_dir_path(const char* path)
{
    int id = fs_resolve(path, root_id);
    if (id >= 0)
        return fs_get_type(id) == FS_DIR ? id : -1;
    return fs_mkdir(path, root_id);
}

void fs_init()
{
    uart_puts("FS: starting persistent mount...\r\n");
    root_id = -1;
    mounted = 0;
    disk_sectors = 0;

    for (int i = 0; i < FS_MAX_NODES; ++i)
        clear_bytes(&nodes[i], sizeof(nodes[i]));
    clear_bytes(bitmap, sizeof(bitmap));

    if (!storage_ready())
    {
        uart_puts("FS: storage unavailable, using empty RAM fallback.\r\n");
        ram_fallback();
        return;
    }

    int rc = load_super();
    if (rc == -2)
    {
        int blank = 1;
        if (storage_read_sector(0, io_buffer) == 0)
        {
            for (int i = 0; i < 512; ++i)
            {
                if (io_buffer[i] != 0)
                {
                    blank = 0;
                    break;
                }
            }
        }
        else
        {
            blank = 0;
        }

        if (blank)
            rc = format_fs();
        else
            rc = -10;
    }

    if (rc != 0)
    {
        uart_puts("FS: mount failed, RAM fallback enabled.\r\n");
        ram_fallback();
        return;
    }

    if (mounted == 0)
    {
        if (load_all_inodes() != 0)
        {
            uart_puts("FS: inode table read failed.\r\n");
            ram_fallback();
            return;
        }
        root_id = 0;

        /* Rebuild the allocation bitmap from authoritative inode extents.
           This also repairs a stale/corrupt on-disk bitmap during mount. */
        if (rebuild_bitmap_from_inodes() != 0)
        {
            uart_puts("FS: extent integrity check failed.\r\n");
            ram_fallback();
            return;
        }

        if (!validate_mounted_fs())
        {
            uart_puts("FS: inode integrity check failed.\r\n");
            ram_fallback();
            return;
        }
        if (write_bitmap() != 0)
        {
            uart_puts("FS: bitmap repair failed.\r\n");
            ram_fallback();
            return;
        }
        mounted = 1;
        uart_puts("FS: filesystem mounted, verified and repaired.\r\n");
    }

    int storage_dir = ensure_dir_path("/storage");
    (void)storage_dir;
    int home_dir = ensure_dir_path("/storage/home");
    (void)home_dir;
    ensure_dir_path("/bin");
    ensure_dir_path("/home");
    ensure_dir_path("/home/user");
}

int fs_get_root() { return root_id; }

int fs_resolve(const char* path, int cwd)
{
    if (path == 0) return -1;
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
        if (str_equal(part, ".")) continue;
        if (str_equal(part, ".."))
        {
            if (current != root_id && current >= 0 && nodes[current].parent >= 0)
                current = nodes[current].parent;
            continue;
        }
        current = find_child(current, part);
        if (current < 0) return -1;
    }
    return current;
}

int fs_mkdir(const char* path, int cwd) { return create_path(path, cwd, FS_DIR); }

int fs_touch(const char* path, int cwd)
{
    int existing = fs_resolve(path, cwd);
    if (existing >= 0)
    {
        if (nodes[existing].type != FS_FILE) return -2;
        nodes[existing].mtime = rtc_now();
        return save_inode(existing) == 0 ? -2 : -4;
    }
    return create_path(path, cwd, FS_FILE);
}

int fs_mkdir_p(const char* path, int cwd)
{
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
        int made = create_node(FS_DIR, part, current);
        if (made < 0) return made;
        current = made;
    }
    return current;
}

int fs_write_data(const char* path, int cwd, const uint8_t* data, uint64_t size)
{
    if (path == 0 || data == 0 || size > FS_MAX_FILE_SIZE)
        return -1;

    int id = fs_resolve(path, cwd);
    int created = 0;
    if (id < 0)
    {
        id = fs_touch(path, cwd);
        created = 1;
    }
    if (id < 0 || nodes[id].type != FS_FILE)
        return -2;

    FsNode old; copy_node(&old, &nodes[id]);
    uint32_t need = (uint32_t)((size + FS_SECTOR_SIZE - 1ULL) / FS_SECTOR_SIZE);
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

int fs_read_file(const char* path, int cwd, uint8_t* buffer, uint64_t capacity, uint64_t* out_size)
{
    if (path == 0 || buffer == 0 || capacity == 0) return -1;
    int id = fs_resolve(path, cwd);
    if (id < 0 || nodes[id].type != FS_FILE) return -2;
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
    if (out_size) *out_size = nodes[id].size;
    return 0;
}

int fs_cat(const char* path, int cwd)
{
    int id = fs_resolve(path, cwd);
    if (id < 0) return -1;
    if (nodes[id].type != FS_FILE) return -2;
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
    int id = fs_resolve(path, cwd);
    if (id < 0) return -1;
    if (id == root_id) return -3;
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

    free_sectors(old.first_sector, old.sector_count);
    if (write_bitmap() != 0)
        return -5;

    return storage_flush();
}

static int rm_node_recursive(int id)
{
    if (id < 0 || id >= FS_MAX_NODES || !nodes[id].used) return -1;
    if (id == root_id) return -3;

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
    free_sectors(old.first_sector, old.sector_count);
    if (write_bitmap() != 0) return -5;
    return storage_flush();
}

int fs_rm_recursive(const char* path, int cwd)
{
    int id = fs_resolve(path, cwd);
    if (id < 0) return -1;
    if (id == root_id) return -3;
    return rm_node_recursive(id);
}

int fs_rename(const char* from, const char* to, int cwd)
{
    int id = fs_resolve(from, cwd);
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

int fs_set_mode(int id, uint32_t mode)
{
    if (id < 0 || id >= FS_MAX_NODES || !nodes[id].used) return -1;
    nodes[id].mode = mode & 0777U;
    nodes[id].mtime = rtc_now();
    return save_inode(id);
}

uint64_t fs_total_sectors()
{
    return disk_sectors;
}

uint64_t fs_used_sectors()
{
    uint64_t used = 0;
    for (int i = 0; i < FS_MAX_NODES; ++i)
        if (nodes[i].used && nodes[i].type == FS_FILE)
            used += nodes[i].sector_count;
    return used;
}

int fs_set_mode_path(const char* path, int cwd, uint32_t mode)
{
    int id = fs_resolve(path, cwd);
    if (id < 0) return -1;
    return fs_set_mode(id, mode);
}

int fs_persistent() { return mounted; }
