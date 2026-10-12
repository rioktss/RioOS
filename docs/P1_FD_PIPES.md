# P1: File descriptors and generic pipes — RioOS 1.7.0

## What changed

- Descriptors now live in bounded tables selected by process PID. A descriptor number from one PID is not automatically valid for another PID.
- An open-file description is separate from a descriptor slot. `dup` and `dup2` share file offset and endpoint lifetime.
- Added syscalls 20–22: `SYS_FD_DUP`, `SYS_FD_DUP2`, and `SYS_FD_PIPE`.
- Added `user_fd_dup`, `user_fd_dup2`, and `user_fd_pipe` helpers in `include/user_api.h`.
- Added bounded 4096-byte non-blocking pipe rings; open file-description lifetime controls pipe reader/writer counts.
- The synchronous EL0 runner selects and releases a descriptor table for each launched program.

## Verification

`bash build.sh --clean` compiles the kernel with warnings treated as errors and runs the host test suite. File descriptor tests exercise separate owner tables, shared offsets, descriptor replacement, empty-pipe `EAGAIN`-style return, successful transfer, EOF after writer closure, and `EPIPE` after reader closure. Syscall tests verify the added syscall dispatch and reject invalid `pipe()` user output pointers. Release verification checks that the new syscall paths and userspace wrappers remain present.

## Explicit limitations

This increment does not claim a complete POSIX process model. `fork()`, `wait()`, descriptor inheritance and concurrent user programs remain deferred because RioOS still uses a single shared EL0 virtual window and executes a user program synchronously on the existing runner. Generic pipes are currently non-blocking and the shell parser has not yet switched arbitrary program pipelines to pipe descriptors. Timer IRQ is not enabled by this feature. Boot and interactive behavior still require QEMU validation on a host with `qemu-system-aarch64`.
