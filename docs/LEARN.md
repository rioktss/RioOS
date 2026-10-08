# Learn OS Development with RioOS

RioOS is not only a codebase to run; it is meant to be a codebase you can **read**.

This guide suggests a practical order for studying the project from boot code to userspace.

## Level 1 — Understand the target

Start by understanding the environment:

- ARM64 / AArch64
- QEMU `virt`
- GICv2
- VirtIO
- Generic Timer

Read:

- [`../docs/BUILD_AND_RUN.md`](./BUILD_AND_RUN.md)
- [`../run.sh`](../run.sh)
- [`../README.md`](../README.md)

Questions to answer:

1. What does QEMU provide to the guest?
2. Which parts of RioOS are CPU-generic and which parts are QEMU/virt-specific?
3. Why does a bare-metal kernel need a linker script?

## Level 2 — Read the boot code

Start with:

```text
boot/boot.S
boot/exceptions.S
linker.ld
```

The goal is to understand how control reaches the kernel and how low-level exception entry is represented.

Useful concepts:

- AArch64 registers
- stack initialization
- link/load addresses
- exception vectors
- EL1 / EL0
- alignment requirements

## Level 3 — Understand kernel initialization

Read:

```text
kernel/kernel.cpp
kernel/version.cpp
kernel/diag.cpp
```

Trace the initialization sequence rather than reading every function at once.

Make a small diagram showing:

```text
boot → kernel entry → hardware init → memory → storage → process support → shell
```

## Level 4 — Memory and MMU

Read:

```text
memory/memory.cpp
memory/mmu.cpp
memory/page_alloc.cpp
include/memory.h
include/mmu.h
include/page_alloc.h
```

Learn these concepts alongside the code:

- virtual vs physical addresses
- page tables
- translation
- page allocation
- memory protection
- alignment

Try changing documentation or host tests before touching the MMU implementation itself.

## Level 5 — UART and shell

Read:

```text
 drivers/uart.cpp
 kernel/shell.cpp
 kernel/commands.cpp
 kernel/history.cpp
 include/shell.h
```

The shell is a great first subsystem because it gives immediate feedback while you learn the rest of the kernel.

Start RioOS and run:

```text
help
version
pwd
ls
mem
ps
sched
```

## Level 6 — Filesystem and persistence

Read:

```text
fs/fs.cpp
fs/vfs.cpp
fs/mkfs_myfs.cpp
drivers/storage.cpp
drivers/virtio_mmio.cpp
drivers/virtio_blk.cpp
```

Then follow the path:

```text
shell command
    ↓
VFS
    ↓
filesystem
    ↓
block storage
    ↓
VirtIO
    ↓
QEMU virtual disk
```

This is a useful exercise in tracing a high-level operation down to hardware.

## Level 7 — Processes and scheduling

Read:

```text
process/process.cpp
process/thread.cpp
process/scheduler.cpp
process/sched_policy.cpp
process/ctx_switch.S
```

Learn the difference between:

- process
- thread
- runnable/ready state
- current execution context
- context switch
- scheduling policy

The scheduler is an active area of the project, so use the tests and current implementation as the source of truth.

## Level 8 — Exceptions, interrupts, and timer

Read:

```text
boot/exceptions.S
kernel/exceptions.cpp
kernel/interrupt.cpp
kernel/timer.cpp
```

Study the flow:

```text
hardware event
    ↓
exception/interrupt vector
    ↓
low-level entry
    ↓
kernel interrupt handler
    ↓
subsystem response
```

This is one of the most important parts of bare-metal development because the kernel must coordinate directly with CPU exception state and interrupt controllers.

## Level 9 — Syscalls and user mode

Read:

```text
kernel/syscall.cpp
process/elf.cpp
process/user.cpp
process/user_entry.S
userspace/user_init.S
userspace/nano.S
userspace/calc.cpp
```

Trace:

```text
ELF user program
      ↓
ELF loader
      ↓
user execution
      ↓
syscall / exception transition
      ↓
kernel service
      ↓
return to user
```

## Level 10 — Network stack and VirtIO network

Read:

```text
drivers/virtio_net.cpp
net/netstack.cpp
include/virtio_net.h
include/netstack.h
```

Start with packet parsing and device transport before trying to expand the protocol stack.

## How to study efficiently

Do not read the whole repository line by line.

Use this loop instead:

```text
Pick one subsystem
      ↓
Read its header
      ↓
Read its implementation
      ↓
Find who calls it
      ↓
Read its test(s)
      ↓
Boot QEMU and observe behaviour
      ↓
Write down what you learned
```

## Good first changes for learners

Low-risk contributions are often documentation and tests:

- improve a subsystem explanation
- add a diagram
- add a host-side regression test
- improve an error message
- document a QEMU assumption
- add a small shell command test

These changes teach the architecture without immediately touching delicate boot or interrupt code.

## Suggested external study topics

To understand RioOS deeply, study these concepts in parallel:

- AArch64 instruction set basics
- ARM exception levels
- page tables and MMU operation
- linker scripts and ELF
- interrupt controllers
- timers
- context switching
- system calls
- filesystems
- VirtIO
- QEMU machine models

RioOS is a practical example; it is not intended to replace architecture manuals, OS textbooks, or QEMU documentation.
