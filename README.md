# RioOS

<p align="center">
  <strong>RioOS — An ARM64 Bare-Metal Operating System from Indonesia</strong><br>
  Built around the custom <strong>MyKernel</strong> kernel and designed to run on <strong>QEMU virt</strong>.
</p>

<p align="center">
  <a href="https://github.com/rioktss/RioOS">Repository</a> ·
  <a href="./docs/BUILD_AND_RUN.md">Build & Run</a> ·
  <a href="./docs/ARCHITECTURE.md">Architecture</a> ·
  <a href="./docs/P0_HARDENING.md">P0 Hardening</a> ·
  <a href="./docs/P1_FD_PIPES.md">P1 FD & Pipes</a> ·
  <a href="./docs/P2_FILESYSTEM_SECURITY.md">P2 Filesystem & Security</a> ·
  <a href="./docs/P3_NETWORKING.md">P3 Networking</a> ·
  <a href="./docs/LEARN.md">Learning Guide</a>
</p>

> 🇮🇩 **Made in Indonesia. Built to be studied, improved, and shared with the world.**

[![Architecture](https://img.shields.io/badge/architecture-ARM64%20%2F%20AArch64-informational)](https://github.com/rioktss/RioOS)
[![Target](https://img.shields.io/badge/target-QEMU%20virt-success)](https://www.qemu.org/)
[![License](https://img.shields.io/badge/license-MIT-blue)](./LICENSE)
[![Status](https://img.shields.io/badge/status-work%20in%20progress-orange)](#project-status)

## What is RioOS?

**RioOS** is a small **ARM64/AArch64 bare-metal operating system project** built from low-level components rather than on top of Linux or another general-purpose operating system.

The project uses **MyKernel** as its kernel and targets the **QEMU `virt` machine**. It is designed both as an operating-system development project and as a practical learning resource for people who want to understand what happens below normal application software.

The repository explores topics such as:

- AArch64 boot and exception handling
- kernel memory management and MMU setup
- UART and interactive shell development
- process metadata, scheduling policy, and cooperative kernel threads
- system calls and user/kernel boundaries
- ELF userspace loading
- virtual filesystems and persistent storage
- VirtIO block and network devices
- Generic Timer and GIC interrupt handling
- host-side tests and release verification

RioOS is intentionally small enough to read and experiment with, while still covering many of the building blocks found in larger operating systems.

## Why RioOS exists

RioOS started as a hands-on project: build an operating system, understand the machine underneath it, and document the process so other people can learn from the source.

The goal is not to compete with Linux, BSD, or commercial operating systems. The goal is to make kernel and bare-metal concepts **visible, reproducible, and approachable**.

## Project status

🚧 **RioOS is still in active development.**

Some subsystems are already implemented and tested, while others are still being hardened or expanded. Runtime behaviour can also depend on the host environment and QEMU version, so the documentation distinguishes source-level capabilities from checks that must be performed on the user's own machine.

## Current capabilities

**Current source baseline: 1.10.0 (P4 SMP/driver/framebuffer foundations).** This increment retains P0-P3 and adds CPU Affinity-0 diagnostics, acquire/release spinlock primitives, tested framebuffer text scrolling/background clearing, stricter VirtIO-Net RX length handling, `smp`/`fbinfo` diagnostics, and GitHub Actions CI. SMP remains disabled, only one CPU is reported online, the framebuffer is a reserved software RAM surface not attached to QEMU display, and timer IRQ remains off during normal boot. The process scheduler limitation remains explicit: process scheduling is still logical metadata, while CPU context switching is currently for cooperative kernel threads. See the [P4 notes](./docs/P4_SMP_DRIVER_HARDENING.md) and earlier P0-P3 documents for detailed scope and limitations.

| Area | Current focus |
|---|---|
| CPU / platform | ARM64 / AArch64, QEMU `virt` |
| Boot | Custom AArch64 boot path |
| Kernel | MyKernel, freestanding C++17 + Assembly |
| Console | UART + interactive shell |
| Memory | MMU, page allocation, memory management |
| Exceptions | AArch64 exception vectors and fault handling |
| Interrupts | GIC support and interrupt handling |
| Timer | Generic Timer support |
| Processes | Process lifecycle and user/kernel execution |
| Scheduler | Logical process scheduler + cooperative kernel-thread context switching (`yield`) |
| Syscalls | Kernel syscall interface, pointer validation, and host tests |
| File descriptors | Per-process tables, `open/read/write/close/seek`, shared-offset `dup/dup2`, standard streams, and bounded I/O |
| IPC primitives | Bounded non-blocking kernel pipes exposed through `pipe/read/write/close` syscalls |
| Userspace | ELF loading + bundled userspace programs |
| Filesystem | Custom filesystem/VFS + persistent disk image |
| Storage | VirtIO block device |
| Networking | VirtIO-Net, ARP/IPv4/ICMP, UDP datagrams, bounded owner-scoped UDP sockets, DNS A lookup |
| Graphics/input | Tested framebuffer RAM-surface rendering/scrolling and keyboard components; not a QEMU video device |
| Shell | Environment expansion, `export`/`unset`, and in-memory `cat`/`echo` pipes to `grep`/`wc` |
| Validation | 13 host-side tests + release verification + GitHub Actions CI |

## Architecture at a glance

```text
                            RioOS
                              │
                              ▼
                   ┌────────────────────┐
                   │      MyKernel      │
                   │      AArch64       │
                   └─────────┬──────────┘
                             │
          ┌──────────────────┼──────────────────┐
          │                  │                  │
          ▼                  ▼                  ▼
       Memory            Processes           Devices
       / MMU             / Scheduler         / Drivers
          │                  │                  │
          ├──── Syscalls ────┤                  │
          │                  │                  ├── UART
          │                  │                  ├── VirtIO Block
          │                  │                  ├── VirtIO Network
          │                  │                  ├── GICv2
          │                  │                  └── Generic Timer
          │                  │
          └──────── ELF Userspace ──────────────┘
                             │
                             ▼
                       QEMU `virt`
```

### Target machine

The default runner configures QEMU roughly as follows:

- Machine: `virt`
- GIC: version 2
- CPU model: `cortex-a72`
- Memory: `128M` by default
- Serial console: `-nographic`
- Storage: VirtIO block
- Network: VirtIO network with QEMU user networking

See the exact runner in [`run.sh`](./run.sh) and [`run.ps1`](./run.ps1).

## Repository layout

```text
RioOS/
├── boot/          # AArch64 boot and exception entry code
├── drivers/       # UART, framebuffer, keyboard, storage, VirtIO drivers
├── fs/             # Filesystem and VFS implementation
├── include/        # Kernel interfaces and shared headers
├── kernel/         # Core kernel services and shell/syscall logic
├── memory/         # MMU, page allocator, memory management
├── net/            # Network stack components
├── process/        # Processes, threads, ELF loader, scheduler
├── storage/        # Persistent disk image and filesystem data
├── tests/           # Host-side tests and release verification
├── userspace/       # Bundled AArch64 user programs
├── build.sh         # Unix-like build entry point
├── build.ps1        # Windows PowerShell build entry point
├── run.sh           # QEMU runner for Unix-like systems
├── run.ps1          # QEMU runner for Windows
├── linker.ld        # Kernel linker script
└── README.md        # Project overview
```

Generated compiler output lives under `build/`. The persistent filesystem image is under `storage/disk.img`.

## Build and run

### Requirements

The standard build expects:

- Clang
- Clang++
- LLD (`ld.lld`)
- Python 3
- QEMU with `qemu-system-aarch64`

The compiler must support the AArch64 bare-metal target:

```text
--target=aarch64-none-elf
```

### Linux / macOS / Termux / WSL

```bash
git clone https://github.com/rioktss/RioOS.git
cd RioOS
chmod +x build.sh run.sh
./build.sh
./run.sh
```

Or build and launch QEMU in one command:

```bash
./build.sh --run
```

Useful build options:

```bash
./build.sh --verbose
./build.sh --clean
./build.sh --help
```

### Windows PowerShell

```powershell
git clone https://github.com/rioktss/RioOS.git
cd RioOS
.\build.ps1
.\run.ps1
```

Or:

```powershell
.\build.ps1 --run
```

See [`docs/BUILD_AND_RUN.md`](./docs/BUILD_AND_RUN.md) for platform-specific setup and troubleshooting.

## First boot

After a successful build and QEMU launch, RioOS provides a serial-console shell. A typical session starts with the MyKernel initialization message and a shell prompt similar to:

```text
MyKernel initialization complete.
Welcome to MyKernel!
Type 'help' to get started.

[home@RioOS ]$
```

The exact boot log can change as development continues.

### Cooperative threads and timer safety

This release links the AArch64 context-switch path and initializes a small cooperative kernel-thread subsystem. Try `threads` to inspect it, or `thread-demo` to run two yielding demo threads. This is **cooperative thread switching**, not full preemptive process multitasking. The periodic Generic Timer IRQ and CPU IRQ unmasking remain disabled during normal boot; `thread-demo` does not enable them.

### P4: SMP/driver foundations and terminal rendering

The 1.10.0 increment adds a CPU-topology diagnostic (`smp`) and acquire/release spinlock primitives as groundwork. It intentionally does **not** start secondary CPUs: per-CPU stacks/state, secondary boot protocol, interrupt routing, and subsystem-wide lock auditing are still required first. The reported online CPU count remains one and `smp_is_enabled()` remains false.

Framebuffer rendering is now host-testable and scrolls one text row instead of clearing the whole surface. `fbinfo` explicitly reports that this is a reserved RAM surface and is not connected to a QEMU video adapter; the serial UART remains the real console. VirtIO-Net now rejects impossible receive lengths and avoids silently truncating Ethernet frames. A GitHub Actions workflow performs clean builds, host tests, and release checks. See [`docs/P4_SMP_DRIVER_HARDENING.md`](./docs/P4_SMP_DRIVER_HARDENING.md).

P4 still does not enable timer IRQs during boot and does not claim SMP-safe drivers, real GUI support, or a complete graphical terminal.

### File descriptors and userspace API

The syscall ABI now includes bounded file-descriptor operations (`SYS_FD_OPEN`, `SYS_FD_CLOSE`, `SYS_FD_READ`, `SYS_FD_WRITE`, and `SYS_FD_SEEK`). The matching inline helpers and open flags are in `include/user_api.h`; the kernel implementation is in `kernel/fd.cpp`. Descriptors 0/1/2 represent non-blocking stdin and UART stdout/stderr. File reads/writes are limited to 4096 bytes per syscall, individual files to the filesystem limit, and descriptors are reset around each EL0 execution because RioOS currently supports one active userspace program at a time. This is a foundational API, not a claim of complete POSIX compatibility or generic kernel pipes. Use `fds` to inspect the current descriptor table.

## Shell and userspace

The repository includes an interactive shell and bundled userspace programs. Useful commands include filesystem navigation, `export`/`unset` and `$VAR` expansion, `cat file | grep pattern`, `cat file | wc -l`, process/thread inspection, system-call tests (`syscall fd` exercises open/write/seek/read/close), ELF execution, and disk/network diagnostics.

Start with:

```text
help
```

For a learning-oriented tour of the shell, ELF userspace, syscalls, and filesystem, see [`docs/LEARN.md`](./docs/LEARN.md).
For details and a userspace example of the new descriptor syscalls, see [`docs/FILE_DESCRIPTORS.md`](./docs/FILE_DESCRIPTORS.md).

## Persistence

RioOS uses a persistent disk image at:

```text
storage/disk.img
```

The build system creates the image when needed and populates it with bundled userspace programs. The image is intentionally kept outside the generated compiler objects so filesystem state can survive a QEMU restart.

The default `.gitignore` excludes generated images and compiler output from Git commits.

## Tests and verification

RioOS includes host-side tests for several subsystems and a release verification script. The test matrix is documented in [`tests/TEST_MATRIX.md`](./tests/TEST_MATRIX.md).

The build pipeline currently includes checks for areas such as:

- filesystem persistence and VFS behaviour
- network parsing
- memory/page allocation helpers
- syscall helpers and file-descriptor API regression tests
- diagnostics
- scheduler policy
- AArch64 kernel ELF properties
- exception-vector alignment/size
- disk-image and ZIP integrity

Full QEMU runtime checks are environment-dependent and should be performed by the developer when validating a new build.

## Documentation

The documentation is intentionally separated from the kernel source so the implementation can stay focused.

| Document | Purpose |
|---|---|
| [`docs/BUILD_AND_RUN.md`](./docs/BUILD_AND_RUN.md) | Build, boot, platform setup, environment overrides, troubleshooting |
| [`docs/ARCHITECTURE.md`](./docs/ARCHITECTURE.md) | Kernel architecture and subsystem relationships |
| [`docs/LEARN.md`](./docs/LEARN.md) | Guided learning path through the source tree |
| [`docs/DEVELOPMENT.md`](./docs/DEVELOPMENT.md) | Development workflow, tests, conventions, and safe changes |
| [`docs/ROADMAP.md`](./docs/ROADMAP.md) | Development direction and future milestones |
| [`docs/FAQ.md`](./docs/FAQ.md) | Common questions about bare metal, QEMU, ARM64, and RioOS |
| [`PORTABLE_BUILD.md`](./PORTABLE_BUILD.md) | Detailed portable build-system reference |
| [`tests/TEST_MATRIX.md`](./tests/TEST_MATRIX.md) | Test and release-verification matrix |

## Learning from the project

RioOS is intended to be useful for people searching for practical examples of:

- how a bare-metal ARM64 kernel is organized
- how an AArch64 system boots without a host OS underneath the kernel
- how a kernel talks to virtual hardware in QEMU
- how memory, processes, interrupts, and system calls fit together
- how a small filesystem and VFS can be integrated into a kernel
- how an ELF userspace program can cross the kernel/user boundary

A good starting point is [`docs/LEARN.md`](./docs/LEARN.md), then follow the linked source files and tests.

## Contributing

RioOS is a learning project, and contributions that improve correctness, documentation, portability, tests, or educational value are welcome.

Before changing kernel code, read the architecture and development guides so that changes remain understandable and easy to review.

See [`docs/DEVELOPMENT.md`](./docs/DEVELOPMENT.md).

## Roadmap

The long-term direction is to make RioOS a more complete, better-tested, and easier-to-study educational ARM64 operating system.

Planned areas include:

- stronger interrupt and timer stability
- scheduler and preemption hardening
- broader driver coverage
- more complete userspace facilities
- stronger filesystem/VFS semantics
- additional networking capabilities
- more automated QEMU testing
- better developer documentation and diagrams

See [`docs/ROADMAP.md`](./docs/ROADMAP.md).

## Philosophy

> **Start small. Understand the machine. Build one subsystem at a time.**

RioOS is not trying to hide the low-level details. The low-level details are the point.

## About the author

**Rio / rioktss** is building RioOS from Indonesia as a personal open-source operating-system project.

GitHub: <https://github.com/rioktss>

Project: <https://github.com/rioktss/RioOS>

The project is shared publicly so that developers around the world can study the code, learn from the experiments, and help improve it.

## License

RioOS is released under the [MIT License](./LICENSE).

---

**RioOS • ARM64 • Bare Metal • QEMU • Open Source • Indonesia 🇮🇩**
