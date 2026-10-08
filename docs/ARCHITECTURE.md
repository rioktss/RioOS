# RioOS Architecture

RioOS is intentionally organized as a small collection of cooperating kernel subsystems. The implementation is written mostly in freestanding C++ with AArch64 Assembly for the lowest-level entry and context-switch paths.

## Design goals

RioOS currently aims to be:

1. small enough to study,
2. explicit about hardware boundaries,
3. easy to boot in QEMU,
4. testable from the host where practical, and
5. useful as a learning reference for OS development.

## System overview

```text
                         ┌─────────────────────┐
                         │      QEMU virt       │
                         │     ARM64 machine    │
                         └──────────┬──────────┘
                                    │
                  ┌─────────────────┼─────────────────┐
                  │                 │                 │
                CPU              Memory            Devices
                  │                 │                 │
                  ▼                 ▼                 ▼
             ┌────────┐      ┌────────────┐     ┌────────────┐
             │  Boot  │─────▶│  MyKernel  │◀────│  Drivers   │
             └────────┘      └─────┬──────┘     └────────────┘
                                   │
             ┌─────────────────────┼──────────────────────┐
             │                     │                      │
             ▼                     ▼                      ▼
          Processes             Filesystem             Syscalls
             │                     │                      │
             ▼                     ▼                      ▼
        Scheduler /           VFS + Storage        ELF / Userspace
        Threads / ELF            │                      │
             │                   ▼                      │
             │              VirtIO block               │
             │                                          │
             └───────────────────┬──────────────────────┘
                                 ▼
                            User programs
```

## Boot path

At a high level, the boot flow is:

```text
QEMU loads kernel
      │
      ▼
boot/boot.S
      │
      ▼
AArch64 early setup
      │
      ├── stack / CPU setup
      ├── early console support
      └── transfer to kernel entry
      │
      ▼
kernel/kernel.cpp
      │
      ▼
subsystem initialization
      │
      ├── memory / MMU
      ├── UART
      ├── filesystem / storage
      ├── interrupts / timer
      ├── processes / scheduler
      ├── network / VirtIO
      └── shell
      │
      ▼
interactive RioOS shell
```

The exact initialization order can change as the kernel evolves. The authoritative implementation is the source tree.

## AArch64 exception and interrupt model

Low-level exception entry is implemented in Assembly and connected to C++ exception/interrupt handling.

Relevant locations include:

```text
boot/exceptions.S
kernel/exceptions.cpp
kernel/interrupt.cpp
kernel/timer.cpp
include/exceptions.h
include/interrupt.h
include/timer.h
```

RioOS targets a QEMU `virt` machine configured with GICv2 and a Generic Timer. This allows the project to experiment with hardware-style interrupt delivery without requiring physical ARM hardware.

## Memory management

Memory-related code is split into focused components:

```text
memory/mmu.cpp
memory/memory.cpp
memory/page_alloc.cpp
include/mmu.h
include/memory.h
include/page_alloc.h
```

The build is freestanding: the kernel is not linked against a normal host C++ runtime.

The project uses the AArch64 bare-metal target:

```text
--target=aarch64-none-elf
```

and builds the kernel with options suitable for a freestanding environment, including disabled C++ exceptions/RTTI and no host standard library.

## Processes, threads, and scheduling

Process-related code is grouped under `process/`:

```text
process/process.cpp
process/thread.cpp
process/scheduler.cpp
process/sched_policy.cpp
process/elf.cpp
process/ctx_switch.S
process/user_entry.S
```

Conceptually:

```text
Process
  ├── address-space / execution state
  ├── threads
  └── scheduling state
          │
          ▼
      Scheduler
          │
          ▼
   context switching
```

The scheduler is an active development area, so the implementation details should be treated as evolving rather than frozen ABI.

## System calls and userspace

The syscall interface is represented by the syscall layer in the kernel and corresponding userspace support.

A simplified path looks like:

```text
User program
    │
    ▼
userspace interface
    │
    ▼
AArch64 exception / syscall entry
    │
    ▼
kernel/syscall.cpp
    │
    ▼
validated kernel operation
    │
    ▼
return to userspace
```

ELF loading is handled by the process/ELF subsystem. Bundled userspace programs are built as AArch64 ELF artifacts by `build.sh` and copied into the filesystem image.

## Filesystem and storage

Filesystem code is separated into filesystem primitives and the virtual filesystem layer:

```text
fs/fs.cpp
fs/vfs.cpp
fs/mkfs_myfs.cpp
include/fs.h
include/vfs.h
```

Storage is exposed through the VirtIO block device path:

```text
QEMU disk
   │
   ▼
VirtIO block
   │
   ▼
storage/ / block driver
   │
   ▼
filesystem
   │
   ▼
VFS
   │
   ▼
shell / userspace-facing file operations
```

The persistent image used by the default runner is:

```text
storage/disk.img
```

## Networking

The network side is organized around the VirtIO network device and a small network stack:

```text
QEMU user networking
        │
        ▼
VirtIO net device
        │
        ▼
drivers/virtio_net.cpp
        │
        ▼
net/netstack.cpp
```

The current networking code is intentionally small and is supported by host-side parser tests.

## Drivers

Hardware-facing code lives primarily under `drivers/`:

```text
uart.cpp
keyboard.cpp
storage.cpp
virtio_mmio.cpp
virtio_blk.cpp
virtio_net.cpp
fb.cpp
font.cpp
```

This separation makes it easier to reason about the boundary between generic kernel services and QEMU/virtio-specific hardware interfaces.

## Host tools and tests

Not everything in the repository runs inside the bare-metal kernel.

Some utilities and tests intentionally compile for the host machine. This gives fast feedback for filesystem, VFS, networking, memory helpers, syscalls, diagnostics, and scheduler policy without booting QEMU for every small change.

The release verification script additionally checks static properties of the kernel image and release artifacts.

See [`../tests/TEST_MATRIX.md`](../tests/TEST_MATRIX.md).

## Source-to-subsystem map

| Question | Start here |
|---|---|
| How does the machine boot? | `boot/`, `boot/boot.S` |
| Where is the kernel entry/init logic? | `kernel/kernel.cpp` |
| How are exceptions handled? | `boot/exceptions.S`, `kernel/exceptions.cpp` |
| How do interrupts work? | `kernel/interrupt.cpp`, `kernel/timer.cpp` |
| How is memory managed? | `memory/`, `include/memory.h`, `include/mmu.h` |
| Where are processes/threads? | `process/` |
| Where is scheduling implemented? | `process/scheduler.cpp`, `process/sched_policy.cpp` |
| Where are syscalls? | `kernel/syscall.cpp`, `include/syscall.h` |
| How are ELF programs loaded? | `process/elf.cpp` |
| Where is the filesystem? | `fs/` |
| Where are block/network drivers? | `drivers/virtio_*.cpp`, `drivers/storage.cpp` |
| How are user programs built? | `userspace/`, `build.sh` |
| How is the kernel linked? | `linker.ld` |
| How is QEMU launched? | `run.sh`, `run.ps1` |

## Architectural philosophy

RioOS deliberately keeps the layers visible:

```text
hardware / QEMU
      ↓
drivers + interrupt entry
      ↓
kernel services
      ↓
processes + filesystem + networking
      ↓
syscalls
      ↓
userspace
```

This makes the project useful for learning because the path from a virtual device to a shell command is traceable in the repository.
