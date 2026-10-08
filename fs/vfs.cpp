#include "vfs.h"
#include "fs.h"
#include "uart.h"
#include "string.h"

static int current_directory = -1;

void vfs_init()
{
    current_directory =
        fs_get_root();

    uart_puts(
        "VFS initialized.\r\n"
    );
}

int vfs_cwd()
{
    return current_directory;
}

void vfs_pwd()
{
    if (current_directory ==
        fs_get_root())
    {
        uart_puts("/\r\n");
        return;
    }

    char parts[32][32];

    int count = 0;

    int current =
        current_directory;

    while (
        current >= 0 &&
        current != fs_get_root() &&
        count < 32
    )
    {
        const char* name =
            fs_get_name(current);

        str_copy(
            parts[count],
            name,
            32
        );

        count++;

        current =
            fs_get_parent(current);
    }

    uart_putc('/');

    for (int i = count - 1;
         i >= 0;
         i--)
    {
        uart_puts(
            parts[i]
        );

        if (i != 0)
            uart_putc('/');
    }

    uart_puts(
        "\r\n"
    );
}

static void put_size(uint64_t value)
{
    char digits[24];
    int n = 0;
    if (value == 0) digits[n++] = '0';
    while (value > 0 && n < 23)
    {
        digits[n++] = (char)('0' + (value % 10ULL));
        value /= 10ULL;
    }
    while (n > 0)
        uart_putc(digits[--n]);
}

static void ls_entry(int id)
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
        put_size(fs_file_size(id));
        uart_puts(" bytes)");
    }
    uart_puts("\r\n");
}

void vfs_ls(const char* path)
{
    int dir = current_directory;

    if (path != 0 && path[0] != '\0')
    {
        dir = fs_resolve(path, current_directory);
        if (dir < 0)
        {
            uart_puts("ls: not found\r\n");
            return;
        }
        if (fs_get_type(dir) != FS_DIR)
        {
            ls_entry(dir);
            return;
        }
    }

    int count = fs_get_child_count(dir);
    if (count == 0)
    {
        uart_puts("(empty)\r\n");
        return;
    }

    for (int i = 0; i < count; i++)
    {
        int child = fs_get_child(dir, i);
        if (child >= 0)
            ls_entry(child);
    }
}

void vfs_get_path(char* out, int capacity)
{
    if (out == 0 || capacity <= 0)
        return;

    if (capacity < 2 || current_directory == fs_get_root() || current_directory < 0)
    {
        out[0] = (capacity >= 2) ? '/' : '\0';
        if (capacity >= 2) out[1] = '\0';
        return;
    }

    int chain[32];
    int count = 0;
    int current = current_directory;
    while (current >= 0 && current != fs_get_root() && count < 32)
    {
        chain[count++] = current;
        current = fs_get_parent(current);
    }

    int pos = 0;
    for (int i = count - 1; i >= 0; --i)
    {
        if (pos < capacity - 1) out[pos++] = '/';
        const char* name = fs_get_name(chain[i]);
        for (int k = 0; name[k] && pos < capacity - 1; ++k)
            out[pos++] = name[k];
    }
    out[pos] = '\0';
}

int vfs_cd(
    const char* path
)
{
    if (path == 0 ||
        path[0] == '\0')
    {
        current_directory =
            fs_get_root();

        return 0;
    }

    int target =
        fs_resolve(
            path,
            current_directory
        );

    if (target < 0)
        return -1;

    if (fs_get_type(target) !=
        FS_DIR)
    {
        return -2;
    }

    current_directory =
        target;

    return 0;
}

int vfs_mkdir(
    const char* path
)
{
    return fs_mkdir(
        path,
        current_directory
    );
}

int vfs_touch(
    const char* path
)
{
    return fs_touch(
        path,
        current_directory
    );
}

int vfs_write(
    const char* path,
    const char* content
)
{
    return fs_write(
        path,
        current_directory,
        content
    );
}

int vfs_cat(
    const char* path
)
{
    return fs_cat(
        path,
        current_directory
    );
}

int vfs_rm(
    const char* path
)
{
    int result = fs_rm(
        path,
        current_directory
    );

    /* "rm ." (or removing a parent of the cwd) must not leave the shell
       standing in a directory that no longer exists. */
    if (result == 0 &&
        fs_get_type(current_directory) != FS_DIR)
    {
        current_directory =
            fs_get_root();
    }

    return result;
}

static void tree_recursive(
    int id,
    int depth
)
{
    for (int i = 0;
         i < depth;
         i++)
    {
        uart_puts(
            "  "
        );
    }

    if (fs_get_type(id) ==
        FS_DIR)
    {
        uart_puts(
            "[DIR]  "
        );
    }
    else
    {
        uart_puts(
            "[FILE] "
        );
    }

    uart_puts(
        fs_get_name(id)
    );

    uart_puts(
        "\r\n"
    );

    int count =
        fs_get_child_count(id);

    for (int i = 0;
         i < count;
         i++)
    {
        int child =
            fs_get_child(
                id,
                i
            );

        if (child >= 0)
        {
            tree_recursive(
                child,
                depth + 1
            );
        }
    }
}

void vfs_tree()
{
    uart_puts(
        "/\r\n"
    );

    int root =
        fs_get_root();

    int count =
        fs_get_child_count(root);

    for (int i = 0;
         i < count;
         i++)
    {
        int child =
            fs_get_child(
                root,
                i
            );

        if (child >= 0)
        {
            tree_recursive(
                child,
                1
            );
        }
    }
}