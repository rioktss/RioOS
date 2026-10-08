#include "vfs.h"
#include "fs.h"
#include "uart.h"
#include "string.h"

static int current_directory = -1;

void vfs_init()
{
    current_directory = fs_get_root();
    uart_puts("VFS initialized.\r\n");
}

int vfs_cwd() { return current_directory; }

void vfs_pwd()
{
    char path[192];
    vfs_get_path(path, (int)sizeof(path));
    uart_puts(path);
    uart_puts("\r\n");
}

static void put_u64(uint64_t value)
{
    char digits[24];
    int n = 0;
    if (value == 0) digits[n++] = '0';
    while (value > 0 && n < (int)sizeof(digits))
    {
        digits[n++] = (char)('0' + (value % 10ULL));
        value /= 10ULL;
    }
    while (n > 0) uart_putc(digits[--n]);
}

static void put_dec2(uint64_t value)
{
    uart_putc((char)('0' + ((value / 10ULL) % 10ULL)));
    uart_putc((char)('0' + (value % 10ULL)));
}

static void put_dec4(uint64_t value)
{
    uart_putc((char)('0' + ((value / 1000ULL) % 10ULL)));
    uart_putc((char)('0' + ((value / 100ULL) % 10ULL)));
    uart_putc((char)('0' + ((value / 10ULL) % 10ULL)));
    uart_putc((char)('0' + (value % 10ULL)));
}

/* Gregorian calendar conversion for Unix epoch seconds. */
static void put_datetime(uint64_t seconds)
{
    uint64_t days = seconds / 86400ULL;
    uint64_t rem = seconds % 86400ULL;
    uint64_t hour = rem / 3600ULL;
    rem %= 3600ULL;
    uint64_t minute = rem / 60ULL;

    /* days -> civil date, valid for the normal RTC range used by QEMU. */
    int64_t z = (int64_t)days + 719468LL;
    int64_t era = (z >= 0 ? z : z - 146096LL) / 146097LL;
    uint64_t doe = (uint64_t)(z - era * 146097LL);
    uint64_t yoe = (doe - doe / 1460ULL + doe / 36524ULL - doe / 146096ULL) / 365ULL;
    int64_t y = era * 400LL + (int64_t)yoe;
    uint64_t doy = doe - (365ULL * yoe + yoe / 4ULL - yoe / 100ULL);
    uint64_t mp = (5ULL * doy + 2ULL) / 153ULL;
    uint64_t day = doy - (153ULL * mp + 2ULL) / 5ULL + 1ULL;
    uint64_t month = mp < 10ULL ? mp + 3ULL : mp - 9ULL;
    y += month <= 2ULL ? 1LL : 0LL;

    if (y < 0) y = 0;
    if (y > 9999) y = 9999;
    put_dec4((uint64_t)y);
    uart_putc('-'); put_dec2(month);
    uart_putc('-'); put_dec2(day);
    uart_putc(' '); put_dec2(hour);
    uart_putc(':'); put_dec2(minute);
}

static void put_permissions(int id)
{
    uint32_t mode = fs_get_mode(id) & 0777U;
    uart_putc(fs_get_type(id) == FS_DIR ? 'd' : '-');
    const uint32_t bits[9] = {0400U,0200U,0100U, 0040U,0020U,0010U, 0004U,0002U,0001U};
    const char chars[9] = {'r','w','x','r','w','x','r','w','x'};
    for (int i = 0; i < 9; ++i)
        uart_putc((mode & bits[i]) ? chars[i] : '-');
}

static void ls_entry_short(int id)
{
    if (fs_get_type(id) == FS_DIR)
    {
        uart_puts("[DIR]  ");
        uart_puts(fs_get_name(id));
    }
    else
    {
        uart_puts("[FILE] ");
        uart_puts(fs_get_name(id));
        uart_puts("  (");
        put_u64(fs_file_size(id));
        uart_puts(" bytes)");
    }
    uart_puts("\r\n");
}

static void ls_entry_long(int id)
{
    put_permissions(id);
    uart_putc(' ');
    put_u64(fs_file_size(id));
    uart_putc(' ');
    put_datetime(fs_get_mtime(id));
    uart_putc(' ');
    uart_puts(fs_get_name(id));
    uart_puts("\r\n");
}

static int name_hidden(int id)
{
    const char* name = fs_get_name(id);
    return name[0] == '.' && name[1] != '\0';
}

static int next_child(int parent, int start_index, int show_hidden)
{
    int count = fs_get_child_count(parent);
    for (int i = start_index + 1; i < count; ++i)
    {
        int child = fs_get_child(parent, i);
        if (child >= 0 && (show_hidden || !name_hidden(child)))
            return child;
    }
    return -1;
}

static void ls_directory(int dir, int long_format, int show_hidden)
{
    int any = 0;
    int previous_index = -1;
    while (1)
    {
        int child = next_child(dir, previous_index, show_hidden);
        if (child < 0) break;
        if (long_format) ls_entry_long(child); else ls_entry_short(child);
        /* Child index, rather than inode id, is the iteration cursor. */
        for (int i = previous_index + 1; i < fs_get_child_count(dir); ++i)
            if (fs_get_child(dir, i) == child) { previous_index = i; break; }
        any = 1;
    }
    if (!any)
        uart_puts("(empty)\r\n");
}

static void build_path(int id, char* out, int cap)
{
    if (out == 0 || cap <= 0) return;
    if (id == fs_get_root() || id < 0) { str_copy(out, "/", cap); return; }

    int chain[64];
    int count = 0;
    int cur = id;
    while (cur >= 0 && cur != fs_get_root() && count < 64)
    {
        chain[count++] = cur;
        cur = fs_get_parent(cur);
    }
    int pos = 0;
    for (int i = count - 1; i >= 0; --i)
    {
        if (pos < cap - 1) out[pos++] = '/';
        const char* name = fs_get_name(chain[i]);
        for (int k = 0; name[k] && pos < cap - 1; ++k)
            out[pos++] = name[k];
    }
    out[pos] = '\0';
}

static void ls_recursive(int dir, int long_format, int show_hidden, const char* display_path, int depth)
{
    if (depth > 64) return;
    if (depth > 0)
    {
        uart_puts(display_path);
        uart_puts(":\r\n");
    }
    ls_directory(dir, long_format, show_hidden);

    int previous_index = -1;
    while (1)
    {
        int child = next_child(dir, previous_index, show_hidden);
        if (child < 0) break;
        for (int i = previous_index + 1; i < fs_get_child_count(dir); ++i)
            if (fs_get_child(dir, i) == child) { previous_index = i; break; }
        if (fs_get_type(child) == FS_DIR)
        {
            char child_path[256];
            if (str_equal(display_path, "/"))
            {
                child_path[0] = '/';
                str_copy(child_path + 1, fs_get_name(child), sizeof(child_path) - 1);
            }
            else
            {
                str_copy(child_path, display_path, sizeof(child_path));
                int n = str_len(child_path);
                if (n < (int)sizeof(child_path) - 1) child_path[n++] = '/';
                str_copy(child_path + n, fs_get_name(child), sizeof(child_path) - n);
            }
            uart_puts("\r\n");
            ls_recursive(child, long_format, show_hidden, child_path, depth + 1);
        }
    }
}

void vfs_ls(const char* spec)
{
    int long_format = 0;
    int show_hidden = 0;
    int recursive = 0;
    char operand[192];
    operand[0] = '\0';

    if (spec != 0)
    {
        const char* p = spec;
        while (*p)
        {
            while (*p == ' ' || *p == '\t') ++p;
            if (!*p) break;
            char token[192];
            int n = 0;
            while (*p && *p != ' ' && *p != '\t')
            {
                if (n >= (int)sizeof(token) - 1) { uart_puts("ls: argument too long\r\n"); return; }
                token[n++] = *p++;
            }
            token[n] = '\0';

            if (token[0] == '-' && token[1] != '\0')
            {
                for (int i = 1; token[i]; ++i)
                {
                    if (token[i] == 'l') long_format = 1;
                    else if (token[i] == 'a') show_hidden = 1;
                    else if (token[i] == 'R') recursive = 1;
                    else if (token[i] == '-') continue;
                    else { uart_puts("ls: invalid option\r\n"); return; }
                }
            }
            else
            {
                if (operand[0] != '\0') { uart_puts("ls: too many operands\r\n"); return; }
                str_copy(operand, token, sizeof(operand));
            }
        }
    }

    int id = operand[0] ? fs_resolve(operand, current_directory) : current_directory;
    if (id < 0) { uart_puts("ls: not found\r\n"); return; }

    if (fs_get_type(id) != FS_DIR)
    {
        if (long_format) ls_entry_long(id); else ls_entry_short(id);
        return;
    }

    if (recursive)
    {
        char path[256];
        build_path(id, path, sizeof(path));
        ls_recursive(id, long_format, show_hidden, path, 0);
    }
    else
    {
        ls_directory(id, long_format, show_hidden);
    }
}

void vfs_get_path(char* out, int capacity)
{
    if (out == 0 || capacity <= 0) return;
    build_path(current_directory, out, capacity);
}

int vfs_cd(const char* path)
{
    if (path == 0 || path[0] == '\0')
    {
        current_directory = fs_get_root();
        return 0;
    }
    int target = fs_resolve(path, current_directory);
    if (target < 0) return -1;
    if (fs_get_type(target) != FS_DIR) return -2;
    current_directory = target;
    return 0;
}

int vfs_mkdir(const char* path) { return fs_mkdir(path, current_directory); }
int vfs_mkdir_p(const char* path) { return fs_mkdir_p(path, current_directory); }
int vfs_touch(const char* path) { return fs_touch(path, current_directory); }
int vfs_write(const char* path, const char* content) { return fs_write(path, current_directory, content); }

int vfs_cat(const char* path)
{
    return fs_cat(path, current_directory);
}

int vfs_cat_numbered(const char* path)
{
    return fs_cat_numbered(path, current_directory);
}

int vfs_rm(const char* path)
{
    int result = fs_rm(path, current_directory);
    if (result == 0 && fs_get_type(current_directory) != FS_DIR)
        current_directory = fs_get_root();
    return result;
}

int vfs_rm_recursive(const char* path)
{
    int result = fs_rm_recursive(path, current_directory);
    if (result == 0 && fs_get_type(current_directory) != FS_DIR)
        current_directory = fs_get_root();
    return result;
}

static void tree_recursive(int id, int depth)
{
    for (int i = 0; i < depth; ++i) uart_puts("  ");
    if (fs_get_type(id) == FS_DIR) uart_puts("[DIR]  ");
    else uart_puts("[FILE] ");
    uart_puts(fs_get_name(id));
    if (fs_get_type(id) == FS_FILE)
    {
        uart_puts("  ("); put_u64(fs_file_size(id)); uart_puts(" bytes)");
    }
    uart_puts("\r\n");

    int count = fs_get_child_count(id);
    for (int i = 0; i < count; ++i)
    {
        int child = fs_get_child(id, i);
        if (child >= 0) tree_recursive(child, depth + 1);
    }
}

void vfs_tree()
{
    uart_puts("/\r\n");
    int root = fs_get_root();
    int count = fs_get_child_count(root);
    for (int i = 0; i < count; ++i)
    {
        int child = fs_get_child(root, i);
        if (child >= 0) tree_recursive(child, 1);
    }
}
