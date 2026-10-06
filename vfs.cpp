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

void vfs_ls()
{
    int count =
        fs_get_child_count(
            current_directory
        );

    if (count == 0)
    {
        uart_puts(
            "(empty)\r\n"
        );

        return;
    }

    for (int i = 0;
         i < count;
         i++)
    {
        int child =
            fs_get_child(
                current_directory,
                i
            );

        if (child < 0)
            continue;

        if (fs_get_type(child) ==
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
            fs_get_name(child)
        );

        uart_puts(
            "\r\n"
        );
    }
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
    return fs_rm(
        path,
        current_directory
    );
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