# RioOS

<p align="center">
  <strong>RioOS — ARM64/AArch64 Bare-Metal Operating System</strong><br>
  Built around the custom MyKernel kernel and designed to boot on QEMU <code>virt</code>.
</p>

<p align="center">
  <a href="https://github.com/rioktss/RioOS">Repository</a> ·
  <a href="./docs/BUILD_AND_RUN.md">Build &amp; Run</a> ·
  <a href="./docs/ARCHITECTURE.md">Architecture</a> ·
  <a href="./docs/LEARN.md">Learning Guide</a> ·
  <a href="./docs/ROADMAP.md">Roadmap</a>
</p>

> 🇮🇩 Made in Indonesia. Built to be studied, improved, and shared.

[![Architecture](https://img.shields.io/badge/architecture-ARM64%20%2F%20AArch64-informational)](https://github.com/rioktss/RioOS)
[![Target](https://img.shields.io/badge/target-QEMU%20virt-success)](https://www.qemu.org/)
[![License](https://img.shields.io/badge/license-MIT-blue)](./docs/LICENSE)
[![Status](https://img.shields.io/badge/status-work%20in%20progress-orange)](#project-status)

## About RioOS

RioOS is an educational **bare-metal ARM64/AArch64 operating-system project**. It boots the custom MyKernel kernel directly in QEMU rather than running as a Linux application. The host operating system supplies the compiler, linker, tests, and QEMU; the guest kernel is freestanding C++17 plus assembly.

**Current source baseline: v1.13.0 — live 100 Hz timer IRQ and a shell-only QEMU window terminal (scrollable with the mouse wheel).** RioOS is under active development. Host-side tests and release checks cover many subsystems, but a successful build is not a substitute for testing the QEMU window on the target machine.

## Quick start

Install a Clang/LLVM toolchain that supports `--target=aarch64-none-elf`, Python 3, and `qemu-system-aarch64`. Then run the scripts for your host platform from the repository root. Every script resolves the repository root automatically, so the current working directory does not affect source/output paths.

### Linux

```bash
bash linux/build.sh
bash linux/run.sh             # Serial shell in the current terminal
bash linux/run-terminal.sh    # Shell in a QEMU window (mouse-wheel scrollback)
```

### Termux on Android

```bash
bash termux/build.sh
bash termux/run.sh
bash termux/run-terminal.sh
```

For a graphical window, start **Termux:X11** first and use a QEMU build with SDL or GTK support. You can select the SDL backend with `RIOOS_DISPLAY=sdl bash termux/run-terminal.sh`.

### macOS

```bash
bash macos/build.sh
bash macos/run.sh
bash macos/run-terminal.sh
```

The macOS window runners default to QEMU's `cocoa` display backend. Set `RIOOS_DISPLAY=sdl` if your QEMU installation uses SDL instead. Your LLVM/Clang must support the bare-metal AArch64 target; Apple's default Clang alone may not provide the full required toolchain.

### Windows PowerShell

```powershell
.\windows\build.ps1
.\windows\run.ps1             # Serial shell
.\windows\run-terminal.ps1    # Shell in a QEMU window (mouse-wheel scrollback)
```

All platform folders contain the same three entry points: `build`, `run`, and `run-terminal`. Windows uses PowerShell `.ps1` scripts; Linux, Termux, and macOS use Bash `.sh` scripts.

## Build options

Pass these flags to the platform's build script:

| Option | Result |
|---|---|
| `--clean` | Remove generated compiler objects and host-test executables before rebuilding. The persistent `storage/disk.img` is not automatically deleted. |
| `--verbose` / `-v` | Show detailed compiler commands/output. |
| `--run` | Build and test, then start the default serial/headless QEMU runner (`run`), not the QEMU window. |
| `--help` / `-h` | Print the available options. |

Examples:

```bash
bash linux/build.sh --clean
bash linux/build.sh --verbose
bash linux/build.sh --run
```

```powershell
.\windows\build.ps1 --clean
.\windows\build.ps1 --verbose
.\windows\build.ps1 --run
```

To customize QEMU or tool paths, use the existing environment overrides such as `CLANG`, `CXX`, `LD`, `PYTHON`, `QEMU`, `RIOOS_MEM`, `RIOOS_CPU`, and `RIOOS_DISPLAY`. More details and troubleshooting are in [`docs/BUILD_AND_RUN.md`](./docs/BUILD_AND_RUN.md) and [`docs/PORTABLE_BUILD.md`](./docs/PORTABLE_BUILD.md).

## What is included

- **AArch64 boot and exceptions:** custom boot path, exception vectors, fault reporting, and GIC support for the QEMU `virt` machine.
- **Memory:** MMU setup, page allocation, and memory diagnostics.
- **Processes and threads:** PID-owned EL0 page tables and physical frames, a guarded 64 KiB user stack, ELF loading into per-process memory, retained EL0 exception/register frames, system calls, and cooperative kernel-thread context switching. A 100 Hz timer IRQ is live in the shell and while EL0 programs run; user processes still run synchronously, and preemptive saved-frame process scheduling is not yet enabled.
- **File descriptors and IPC:** bounded per-process descriptor tables, standard streams, `open/read/write/close/seek`, `dup/dup2`, and bounded non-blocking kernel pipes.
- **Filesystem and storage:** custom filesystem/VFS and a persistent disk image attached through VirtIO block.
- **Networking:** VirtIO-Net with Ethernet, ARP/IPv4/ICMP, owner-scoped UDP/DNS, and a bounded outbound TCP client. The `httpget <host> [path]` command tests real plain-HTTP access through QEMU user networking; HTTPS/TLS, DHCP, and IPv6 are not included.
- **Shell:** environment expansion, `export`/`unset`, basic filesystem commands, and in-memory `cat`/`echo` pipelines to `grep`/`wc`.
- **Terminal window:** QEMU `ramfb` text terminal (DejaVu Sans Mono, Linux-console palette, 2000-line scrollback, mouse-wheel / Shift+PageUp scrolling). It shows only the shell - there is no GUI.
- **Input:** the window runner attaches VirtIO keyboard and tablet devices (the tablet delivers the wheel without a mouse grab). UART remains available.
- **Validation:** host-side regression tests, release verification, and a GitHub Actions CI workflow.

## Choosing a run mode

| Entry point | What opens |
|---|---|
| `run` | The original serial shell in a headless QEMU session; use the host terminal for input/output. |
| `run-terminal` | The same shell shown in a **QEMU window** by the kernel's framebuffer terminal: black background, DejaVu Sans Mono, scroll with the mouse wheel. The serial console also stays on the host terminal. |

There is no pixel UI, launcher or `.rpk` app any more. Both modes run the same RioOS shell (not GNU Bash). In the window, type with the keyboard, scroll history with the **mouse wheel** (or **Shift+PageUp/PageDown**); any key or new output returns to the bottom. See [`docs/TERMINAL.md`](./docs/TERMINAL.md).

## Project layout

```text
RioOS/
├── README.md                 # Main project overview and quick start
├── linux/                    # build.sh, run.sh, run-terminal.sh
├── termux/                   # Android/Termux Bash entry points
├── macos/                    # macOS Bash entry points
├── windows/                  # build.ps1, run.ps1, run-terminal.ps1
├── boot/ drivers/ include/   # Boot code, device drivers, public headers
├── kernel/ memory/ process/  # Kernel, memory manager, scheduler/process code
├── fs/ net/ userspace/       # Filesystem, network stack, bundled userspace
├── tools/                    # Font generator for the terminal
├── tests/                    # Host regression tests and release checks
├── docs/                     # Architecture, build, development, and feature guides
├── build/                    # Generated compiler outputs
└── storage/                  # Persistent virtual disk image and user data
```

## Known scope and limitations

RioOS is an experimental learning project, not a replacement for Linux/BSD or a ready-to-use general-purpose OS. QEMU display backends and input devices vary by host package. SMP startup remains disabled; the timer IRQ is live (100 Hz tick) but there is no preemptive process switching; per-process address-space isolation is implemented, but saved EL0 frames are not yet switched between concurrently running user processes; and a QEMU visual/input session should be tested locally when changing the renderer or runners. Read the feature-specific guides for exact implementation status and tests.

## Documentation

- [`docs/BUILD_AND_RUN.md`](./docs/BUILD_AND_RUN.md) — dependencies, commands, platform setup, and troubleshooting.
- [`docs/PORTABLE_BUILD.md`](./docs/PORTABLE_BUILD.md) — build-system details and environment overrides.
- [`docs/ARCHITECTURE.md`](./docs/ARCHITECTURE.md) — kernel architecture and subsystem relationships.
- [`docs/TERMINAL.md`](./docs/TERMINAL.md) — the QEMU window terminal (font, scrollback, input).
- [`docs/TIMER_IRQ.md`](./docs/TIMER_IRQ.md) — the live timer IRQ, including while EL0 programs run.
- [`docs/DEVELOPMENT.md`](./docs/DEVELOPMENT.md) — development workflow and safe changes.
- [`docs/LEARN.md`](./docs/LEARN.md) — guided learning path.
- [`docs/ROADMAP.md`](./docs/ROADMAP.md) — planned work.
- [`docs/P5_PROCESS_ISOLATION.md`](./docs/P5_PROCESS_ISOLATION.md) — P5 address spaces, page ownership, context frames, and limitations.
- [`docs/FAQ.md`](./docs/FAQ.md) — common questions.
- [`tests/TEST_MATRIX.md`](./tests/TEST_MATRIX.md) — general test matrix.
- [`docs/TEST_MATRIX_P5.md`](./docs/TEST_MATRIX_P5.md) — P5-specific verification matrix.

## License

RioOS is released under the [MIT License](./docs/LICENSE).

---

**RioOS • MyKernel • ARM64 • Bare Metal • QEMU • Open Source • Indonesia 🇮🇩**
