# Build and Run RioOS

This guide explains how to build RioOS, boot it in QEMU, and troubleshoot common environment problems.

RioOS is a bare-metal ARM64/AArch64 project. Your host operating system provides the compiler, build tools, and QEMU; the kernel itself runs as the guest software.

## 1. Requirements

The build expects these commands to be available in `PATH`:

```text
clang
clang++
ld.lld
python3 (or python)
qemu-system-aarch64
```

The compiler must support:

```text
--target=aarch64-none-elf
```

Check your environment:

```bash
clang --version
clang++ --version
ld.lld --version
python3 --version
qemu-system-aarch64 --version
```

## 2. Clone the repository

```bash
git clone https://github.com/rioktss/RioOS.git
cd RioOS
```

## 3. Build on Linux, macOS, Termux, or WSL

Make the scripts executable:

```bash
chmod +x build.sh run.sh
```

Build:

```bash
./build.sh
```

Build and immediately boot QEMU:

```bash
./build.sh --run
```

Run QEMU separately after a successful build:

```bash
./run.sh
```

## 4. Arch Linux

Install the main dependencies:

```bash
sudo pacman -Syu
sudo pacman -S clang lld python qemu-desktop
```

Then build:

```bash
chmod +x build.sh run.sh
./build.sh
./run.sh
```

## 5. Debian / Ubuntu

Install the equivalent LLVM/Clang, LLD, Python 3, and QEMU packages provided by your distribution.

Verify:

```bash
clang --version
clang++ --version
ld.lld --version
python3 --version
qemu-system-aarch64 --version
```

Then:

```bash
chmod +x build.sh run.sh
./build.sh
./run.sh
```

## 6. Termux / Android

Termux can be used as a development host when the required compiler, linker, Python, and QEMU packages are available in the user's environment.

After installing the dependencies:

```bash
clang --version
clang++ --version
ld.lld --version
python3 --version
qemu-system-aarch64 --version
```

Then:

```bash
chmod +x build.sh run.sh
./build.sh
./run.sh
```

If QEMU cannot access the expected acceleration features on a particular Android device, software emulation may still be possible but can be slower or unavailable depending on the Termux/QEMU build.

## 7. Windows PowerShell

Install LLVM/Clang, LLD, Python 3, and QEMU. Ensure their executable directories are available in `PATH`.

Build:

```powershell
.\build.ps1
```

Build and run:

```powershell
.\build.ps1 --run
```

Run separately:

```powershell
.\run.ps1
```

Optional:

```powershell
.\build.ps1 --verbose
.\build.ps1 --clean
```

## 8. QEMU configuration

The default Unix and PowerShell runners use these important settings:

```text
Machine       : virt
GIC           : v2
CPU           : cortex-a72
Memory        : 128M
Console       : nographic
Kernel        : build/kernel.elf
Disk          : storage/disk.img
Storage       : virtio-blk-device
Network       : virtio-net-device + user networking
```

The source of truth is the runner script itself:

- [`run.sh`](../run.sh)
- [`run.ps1`](../run.ps1)

## 9. Build outputs

After a successful build, the important generated files are:

```text
build/kernel.elf
build/user_init.elf
build/nano.elf
build/calc.elf
storage/disk.img
```

Not every optional userspace artifact is guaranteed to exist in every future revision; consult `build.sh` for the current build flow.

## 10. Environment overrides

The build scripts support tool overrides through environment variables.

Example:

```bash
CLANG=/path/to/clang \
CXX=/path/to/clang++ \
LD=/path/to/ld.lld \
QEMU=/path/to/qemu-system-aarch64 \
./build.sh
```

The QEMU runners also support:

```bash
RIOOS_MEM=128M
RIOOS_CPU=cortex-a72
```

Example:

```bash
RIOOS_MEM=256M RIOOS_CPU=cortex-a72 ./run.sh
```

## 11. What the build script does

At a high level, `build.sh` performs these stages:

```text
1. Build userspace programs
2. Build the host filesystem formatter/tools
3. Prepare storage/disk.img
4. Compile boot and exception entry code
5. Compile the kernel modules
6. Link build/kernel.elf
7. Inspect/verify the kernel ELF
8. Run host-side tests
9. Run release verification
```

The build intentionally separates host tools/tests from the freestanding ARM64 kernel.

## 12. First boot

When QEMU starts, RioOS uses the serial console. A typical boot ends at an interactive shell prompt similar to:

```text
MyKernel initialization complete.
Welcome to MyKernel!
Type 'help' to get started.

[home@RioOS ]$
```

Start with:

```text
help
version
mem
ls
pwd
ps
sched
```

The available command set may change as RioOS evolves.

## 13. Troubleshooting

### `qemu-system-aarch64: command not found`

Install QEMU for your platform or point `QEMU` at the executable:

```bash
QEMU=/custom/path/qemu-system-aarch64 ./run.sh
```

### `clang: command not found`

Install LLVM/Clang and make sure `clang` and `clang++` are in `PATH`.

### `ld.lld: command not found`

Install the LLVM LLD package and verify:

```bash
ld.lld --version
```

### `build/kernel.elf not found`

Run the build first:

```bash
./build.sh
```

### `storage/disk.img not found`

Run the build first. The build system creates and prepares the persistent disk image when needed.

### Build succeeds but QEMU behaves differently

QEMU version, host platform, terminal handling, and acceleration support can affect runtime behaviour. Compare your environment against the QEMU configuration documented above and reproduce the problem from a clean build when possible.

## 14. Cleaning generated output

On Unix-like systems:

```bash
./build.sh --clean
```

On Windows:

```powershell
.\build.ps1 --clean
```

Cleaning removes generated build/test artifacts; it is separate from the source tree.

## P0 hardening release

The 1.6.1 build includes process-manager and logical scheduler regression tests, stricter ELF load-segment/page-permission validation, and syscall pointer-scan overflow guards. Normal boot keeps timer IRQ disabled. Review [P0_HARDENING.md](./P0_HARDENING.md) for verified scope and known scheduler limits.


## P1 descriptor and pipes release (v1.7.0)

Version 1.7.0 adds process-PID-scoped descriptor tables, shared open-file descriptions for `dup`/`dup2`, and a bounded non-blocking kernel pipe syscall. Run `bash build.sh` to execute the new host regressions. See [P1_FD_PIPES.md](./P1_FD_PIPES.md) for behavior and limitations. This does not add `fork()`/`wait()` or concurrent EL0 process execution; timer IRQ remains disabled at boot.

## P2 filesystem and permissions (v1.8.0)

The filesystem persists owner UID/GID in unused inode metadata bytes and migrates older images without changing their inode size. Standard user identity is UID/GID `1000`; root is UID/GID `0`. The development root password is `rioos-root` (embedded demo credential; not production security).

New commands: `stat <path>`, `chown <uid> <gid> <path>` (root only), and `truncate <path> <size>` (up to 262144 bytes; extension is zero-filled). `chmod` now requires file ownership or root. File reads/writes, parent-directory mutations, path traversal, and ELF execution check mode bits. `diskread`, `diskwrite`, and `diskflush` require root.

P2 userspace syscall helpers are `user_fs_stat`, `user_fs_truncate`, and `user_fs_chmod` in `include/user_api.h`. Run `bash build.sh --clean` to rebuild and execute filesystem permission/migration and syscall regression tests. The kernel still uses a single global shell credential context, so this is educational access control rather than a hardened multi-user security boundary. Timer IRQ stays off at boot.

## P3 UDP and DNS networking (v1.9.0)

P3 adds owner-PID-scoped UDP handles, bound/ephemeral ports, bounded receive queues, IPv4/UDP validation and UDP transmit checksum, DNS A-record queries, network syscall helpers, and the `ifconfig`, `nslookup`, and root-only `dnsserver` commands. The default DNS address is `10.0.2.3` for QEMU user-mode networking. See [P3 networking notes](./P3_NETWORKING.md) for API use and limits.

Run `bash build.sh --clean` for the AArch64 clean build and host regressions. On a machine with `qemu-system-aarch64`, also run `net`, `ping 10.0.2.2`, and `nslookup example.com`. Host tests use a fake NIC/DNS peer and are not a replacement for that QEMU runtime test. TCP, DHCP, IPv6, and IP fragment reassembly are not included, and timer IRQ remains disabled at boot.


## P4 SMP/driver foundations (v1.10.0)

Run `bash build.sh --clean` (or `.\build.ps1 --clean` in PowerShell) to build the new SMP and framebuffer host regressions alongside the existing suite. There are 14 host-test executables. The release check verifies SMP stays disabled, the framebuffer host test is present, VirtIO-Net RX buffers are bounded, and timer IRQ has not been enabled in the normal boot path.

After launching QEMU on a host with `qemu-system-aarch64`, try `smp` and `fbinfo`. `smp` should report one online CPU and disabled SMP. `fbinfo` explains that the software framebuffer is not yet connected to a QEMU display; the UART remains the interactive console. Keep timer IRQ disabled during ordinary boot.


## Pixel UI and RPK apps (v1.11.0)

The default `run.sh` / `run.ps1` remains a headless `-nographic` serial shell. To use the graphical framebuffer, build normally and launch `./run-ui.sh` on a QEMU build with GTK/SDL display support (PowerShell: `./run-ui.ps1`). The GUI runner attaches `-device ramfb`; the kernel locates `etc/ramfb` through QEMU `fw_cfg`, configures an XRGB8888 surface, and flushes rendered pixels from cache. The framebuffer is 1024×768.

The graphical runner attaches VirtIO keyboard/tablet devices. Focus the QEMU window to type, swipe/drag upward to unlock, and click a home-screen app icon. Home navigation also supports A/D, W/S, arrow keys, Enter, and 1–5; X returns to the serial shell. UART input is retained as a fallback. The pixel Terminal forwards commands to the same `execute_command()` dispatcher used by the serial shell; it is not a GNU Bash process.

Build installs `/apps/terminal.rpk`, `/apps/calculator.rpk`, `/apps/clock.rpk`, `/apps/files.rpk`, and `/apps/launcher.rpk`. RPK v1 is a manifest format for built-in apps, not a native executable/package loader. The implementations are linked into the kernel. Host tests verify pixel drawing bounds and RPK disk-image installation. They do not replace a visual QEMU runtime test; if `ramfb` is unavailable, RioOS reports that and falls back to the serial shell. Timer IRQ remains disabled.
