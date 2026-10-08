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
