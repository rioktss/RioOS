#ifndef RIOOS_FD_H
#define RIOOS_FD_H

#include "types.h"

#define FD_MAX_DESCRIPTORS 24
#define FD_IO_MAX 4096ULL
#define FD_FILE_MAX (1024ULL * 1024ULL)
#define FD_PIPE_CAPACITY 4096

#define FD_O_RDONLY 0x000U
#define FD_O_WRONLY 0x001U
#define FD_O_RDWR   0x002U
#define FD_O_CREAT  0x100U
#define FD_O_TRUNC  0x200U
#define FD_O_APPEND 0x400U

void fd_init();
void fd_close_all();
int fd_set_process(int pid);
int fd_current_process();
void fd_release_process(int pid);
int fd_open(const char* path, uint32_t flags, int cwd);
int fd_close(int descriptor);
int fd_dup(int descriptor);
int fd_dup2(int old_descriptor, int new_descriptor);
/* Non-blocking bounded pipe. Returns 0 on success and fills descriptors[0/1]. */
int fd_pipe(int descriptors[2]);
int fd_close_all_for(int pid);
int64_t fd_read(int descriptor, void* buffer, uint64_t length);
int64_t fd_write(int descriptor, const void* buffer, uint64_t length);
int64_t fd_seek(int descriptor, int64_t offset, int whence);
void fd_list();

#endif
