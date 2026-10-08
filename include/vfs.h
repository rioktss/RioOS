#ifndef VFS_H
#define VFS_H

void vfs_init();

int vfs_cwd();

void vfs_pwd();
void vfs_ls(const char* path = 0);

/* Absolute path of the current directory into out (always NUL-terminated). */
void vfs_get_path(char* out, int capacity);
void vfs_tree();

int vfs_cd(
    const char* path
);

int vfs_mkdir(
    const char* path
);

int vfs_touch(
    const char* path
);

int vfs_write(
    const char* path,
    const char* content
);

int vfs_cat(
    const char* path
);

int vfs_rm(
    const char* path
);

#endif