# RioOS Documentation

Welcome to the RioOS documentation.

RioOS is an ARM64/AArch64 bare-metal operating system project built around the MyKernel kernel and designed for QEMU `virt`.

## Start here

| Guide | What it covers |
|---|---|
| [`BUILD_AND_RUN.md`](./BUILD_AND_RUN.md) | Install tools, build the kernel, launch QEMU, troubleshoot |
| [`ARCHITECTURE.md`](./ARCHITECTURE.md) | Understand the kernel and subsystem relationships |
| [`LEARN.md`](./LEARN.md) | Follow a practical study path from boot to userspace |
| [`DEVELOPMENT.md`](./DEVELOPMENT.md) | Tests, debugging, coding workflow, and contributions |
| [`ROADMAP.md`](./ROADMAP.md) | Current direction and future work |
| [`FAQ.md`](./FAQ.md) | Common questions about RioOS and bare-metal development |

## Source of truth

The documentation explains the intended architecture and workflow, but the source code remains the final authority when implementation details change.

Start from the repository root:

```text
README.md
boot/
drivers/
fs/
kernel/
memory/
net/
process/
tests/
userspace/
```
