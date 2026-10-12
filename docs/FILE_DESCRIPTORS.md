# RioOS file-descriptor API — v1.7.0

RioOS provides a bounded descriptor API for EL0 userspace. It is a small Unix-inspired interface, not a complete POSIX implementation.

## Operations

| Operation | Syscall | Userspace helper |
|---|---:|---|
| Open a path | `SYS_FD_OPEN` (15) | `user_fd_open(path, flags)` |
| Close a descriptor | `SYS_FD_CLOSE` (16) | `user_fd_close(fd)` |
| Read bytes | `SYS_FD_READ` (17) | `user_fd_read(fd, buffer, length)` |
| Write bytes | `SYS_FD_WRITE` (18) | `user_fd_write(fd, buffer, length)` |
| Seek a file | `SYS_FD_SEEK` (19) | `user_fd_seek(fd, offset, whence)` |
| Duplicate a descriptor | `SYS_FD_DUP` (20) | `user_fd_dup(fd)` |
| Duplicate to a chosen slot | `SYS_FD_DUP2` (21) | `user_fd_dup2(old_fd, new_fd)` |
| Create a pipe | `SYS_FD_PIPE` (22) | `user_fd_pipe(int descriptors[2])` |

Open flags are in `include/user_api.h` and prefixed with `USER_FD_`: `O_RDONLY`, `O_WRONLY`, `O_RDWR`, `O_CREAT`, `O_TRUNC`, and `O_APPEND` equivalents.

## Per-process ownership

Each active process PID gets its own descriptor table, with descriptor slots `0`, `1`, and `2` initially assigned to stdin, stdout, and stderr. A descriptor in one process table is not valid merely because the same number exists in another table. Tables are released when the synchronous EL0 program returns. RioOS does not yet support concurrent user processes or descriptor inheritance through `fork()`; those features depend on independent process address spaces and saved CPU contexts.

`dup` and `dup2` reference the same open-file description, so file offset changes through one duplicate are visible through the others. `dup2` closes the target descriptor first when necessary, and supports replacing descriptors `0`, `1`, or `2` for redirection-style use.

## Bounded non-blocking pipes

`user_fd_pipe(fds)` creates a read endpoint in `fds[0]` and a write endpoint in `fds[1]`. Reads and writes use the ordinary descriptor syscalls. Pipes are byte streams with a 4096-byte ring buffer and currently use non-blocking behavior:

- Empty pipe with at least one writer: `-11` (`EAGAIN`-style result).
- Empty pipe after all write endpoints close: `0` (EOF).
- Full pipe: write returns `-11` when no space remains; a partly full pipe accepts a partial write.
- Pipe with no read endpoints: write returns `-32` (`EPIPE`-style result).
- Pipe storage is reclaimed when both read and write endpoints have no open descriptions.

Pipes are kernel primitives, but the shell's existing textual pipeline parser has not yet been rewritten to execute arbitrary ELF processes through these descriptors. The current user-process execution model remains synchronous.

## Limits and error handling

- 24 descriptor slots per process, with new file descriptors normally allocated from slot `3` upward.
- 32 descriptor tables maximum, reflecting the bounded process table.
- 128 simultaneous open-file descriptions and 8 pipe objects.
- Maximum read/write request: 4096 bytes per syscall.
- Maximum file size handled by this descriptor layer: 256 KiB.
- Standard input uses non-blocking UART polling.
- Error values are returned as negative errno-like numbers.
- Syscall paths validate user memory before copying path strings, read destinations, write sources, and `pipe()` output arrays.

## Example

```cpp
#include "user_api.h"

int pipe_example()
{
    int fds[2] = {-1, -1};
    if (user_fd_pipe(fds) != 0) return -1;

    static const char message[] = "pipe works";
    char buffer[sizeof(message)];
    int64_t sent = user_fd_write(fds[1], message, sizeof(message) - 1);
    int64_t got = user_fd_read(fds[0], buffer, sizeof(message) - 1);
    user_fd_close(fds[1]);
    user_fd_close(fds[0]);
    return (sent == (int64_t)(sizeof(message) - 1) && got == sent) ? 0 : -1;
}
```

Because pipes are non-blocking, a read can return `-11` when no bytes are ready. This example assumes the write succeeds before the read and should not be used as a blocking-pipe compatibility claim.

## Validation

Run `bash build.sh` to clean-build the AArch64 kernel and run host tests. The expanded host tests cover per-PID descriptor isolation, shared offsets via `dup`/`dup2`, pipe data transfer, non-blocking empty reads, EOF, `EPIPE`, and syscall argument validation. A successful host build does not replace an interactive QEMU boot test. Timer IRQ remains off by default.
