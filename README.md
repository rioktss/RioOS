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

**Current source baseline: v1.11.0 — QEMU pixel UI foundation.** RioOS is under active development. Host-side tests and release checks cover many subsystems, but a successful build is not a substitute for testing the QEMU window on the target machine.

## Quick start

Install a Clang/LLVM toolchain that supports `--target=aarch64-none-elf`, Python 3, and `qemu-system-aarch64`. Then run the scripts for your host platform from the repository root. Every script resolves the repository root automatically, so the current working directory does not affect source/output paths.

### Linux

```bash
bash linux/build.sh
bash linux/run.sh             # Serial shell in the current terminal
bash linux/run-terminal.sh    # Plain black text shell in a QEMU window
bash linux/run-ui.sh          # Pixel-rendered lock screen / home UI
```

### Termux on Android

```bash
bash termux/build.sh
bash termux/run.sh
bash termux/run-terminal.sh
bash termux/run-ui.sh
```

For a graphical window, start **Termux:X11** first and use a QEMU build with SDL or GTK support. You can select the SDL backend with `RIOOS_DISPLAY=sdl bash termux/run-ui.sh` or the text-window runner.

### macOS

```bash
bash macos/build.sh
bash macos/run.sh
bash macos/run-terminal.sh
bash macos/run-ui.sh
```

The macOS window runners default to QEMU's `cocoa` display backend. Set `RIOOS_DISPLAY=sdl` if your QEMU installation uses SDL instead. Your LLVM/Clang must support the bare-metal AArch64 target; Apple's default Clang alone may not provide the full required toolchain.

### Windows PowerShell

```powershell
.\windows\build.ps1
.\windows\run.ps1             # Serial shell
.\windows\run-terminal.ps1    # Plain black text shell in a QEMU window
.\windows\run-ui.ps1          # Pixel-rendered UI
```

All platform folders contain the same four entry points: `build`, `run`, `run-terminal`, and `run-ui`. Windows uses PowerShell `.ps1` scripts; Linux, Termux, and macOS use Bash `.sh` scripts.

## Build options

Pass these flags to the platform's build script:

| Option | Result |
|---|---|
| `--clean` | Remove generated compiler objects and host-test executables before rebuilding. The persistent `storage/disk.img` is not automatically deleted. |
| `--verbose` / `-v` | Show detailed compiler commands/output. |
| `--run` | Build and test, then start the default serial/headless QEMU runner (`run`), not the pixel UI. |
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
- **Processes and threads:** process metadata and scheduling policy, ELF loading, system calls, and cooperative kernel-thread context switching. This is not full preemptive process multitasking.
- **File descriptors and IPC:** bounded per-process descriptor tables, standard streams, `open/read/write/close/seek`, `dup/dup2`, and bounded non-blocking kernel pipes.
- **Filesystem and storage:** custom filesystem/VFS and a persistent disk image attached through VirtIO block.
- **Networking:** VirtIO-Net foundations with Ethernet, ARP/IPv4/ICMP, UDP datagrams, owner-scoped UDP sockets, and DNS A-record lookup. TCP, DHCP, and IPv6 are not currently included.
- **Shell:** environment expansion, `export`/`unset`, basic filesystem commands, and in-memory `cat`/`echo` pipelines to `grep`/`wc`.
- **Pixel UI:** QEMU `ramfb` rendering, boot splash, Android/MIUI-inspired lock screen and launcher, Control Centre, settings, local BMP wallpapers/icons, and seven built-in `.rpk` app manifests: Terminal, Calculator, Clock, Files, Launcher, Wallpaper, and Pengaturan.
- **Input:** the graphical runner attaches VirtIO keyboard/tablet devices, and a VirtIO mouse device when available. UART remains a fallback.
- **Validation:** host-side regression tests, release verification, and a GitHub Actions CI workflow.

## Choosing a run mode

| Entry point | What opens |
|---|---|
| `run` | The original serial shell in a headless QEMU session; use the host terminal for input/output. |
| `run-terminal` | The original RioOS shell in a plain black text-console **window**. It does not enable the pixel UI and does not open the `.rpk` Terminal app. |
| `run-ui` | The pixel-rendered lock/home UI and built-in apps through QEMU `ramfb`. |

The pixel Terminal app and the serial shell share RioOS's command dispatcher; neither is GNU Bash. In the graphical UI, focus the QEMU window to use keyboard/tablet input. Swipe or drag upward to unlock, select an app, and use **Esc** to request Back. Home navigation supports **A/D**, **W/S**, arrow keys, **Enter**, **1–7**, **C** for Control Centre, and **X** to return to the serial shell.

## Project layout

```text
RioOS/
├── README.md                 # Main project overview and quick start
├── linux/                    # build.sh, run.sh, run-terminal.sh, run-ui.sh
├── termux/                   # Android/Termux Bash entry points
├── macos/                    # macOS Bash entry points
├── windows/                  # build.ps1, run.ps1, run-terminal.ps1, run-ui.ps1
├── boot/ drivers/ include/   # Boot code, device drivers, public headers
├── kernel/ memory/ process/  # Kernel, memory manager, scheduler/process code
├── fs/ net/ userspace/       # Filesystem, network stack, bundled userspace
├── ui/ apps/ assets/         # Pixel UI, app manifests, icons, wallpapers
├── tests/                    # Host regression tests and release checks
├── docs/                     # Architecture, build, development, and feature guides
├── build/                    # Generated compiler outputs
└── storage/                  # Persistent virtual disk image and user data
```

## Known scope and limitations

RioOS is an experimental learning project, not a replacement for Linux/BSD or a ready-to-use general-purpose OS. QEMU display backends and input devices vary by host package. SMP startup remains disabled; timer IRQ/preemption is not enabled during normal boot; process scheduling is not equivalent to fully concurrent user processes; and a QEMU visual/input session should be tested locally when changing the renderer or runners. Read the feature-specific guides for exact implementation status and tests.

## Documentation

- [`docs/BUILD_AND_RUN.md`](./docs/BUILD_AND_RUN.md) — dependencies, commands, platform setup, and troubleshooting.
- [`docs/PORTABLE_BUILD.md`](./docs/PORTABLE_BUILD.md) — build-system details and environment overrides.
- [`docs/ARCHITECTURE.md`](./docs/ARCHITECTURE.md) — kernel architecture and subsystem relationships.
- [`docs/ANDROID_UI.md`](./docs/ANDROID_UI.md) and [`docs/PIXEL_UI.md`](./docs/PIXEL_UI.md) — graphical UI and customization.
- [`docs/DEVELOPMENT.md`](./docs/DEVELOPMENT.md) — development workflow and safe changes.
- [`docs/LEARN.md`](./docs/LEARN.md) — guided learning path.
- [`docs/ROADMAP.md`](./docs/ROADMAP.md) — planned work.
- [`docs/FAQ.md`](./docs/FAQ.md) — common questions.
- [`tests/TEST_MATRIX.md`](./tests/TEST_MATRIX.md) — test matrix.

## License

RioOS is released under the [MIT License](./docs/LICENSE).

---

**RioOS • MyKernel • ARM64 • Bare Metal • QEMU • Open Source • Indonesia 🇮🇩**
