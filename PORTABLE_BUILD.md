# RioOS Portable Build System v2.0

This package makes the RioOS build/run workflow portable across:

- Termux
- Linux
- macOS
- WSL
- Windows PowerShell

## Files

- `build.sh` — portable Bash build script
- `build.ps1` — native Windows PowerShell build script
- `run.sh` — QEMU runner for Unix-like systems
- `run.ps1` — QEMU runner for Windows
- `PORTABLE_BUILD.md` — this documentation

## Important

This package does **not** remove or replace RioOS source files. Extract it into the RioOS repository root and keep the existing kernel/source/test layout.

Expected existing paths include:

```text
boot/
include/
kernel/
memory/
process/
fs/
net/
tests/
userspace/
linker.ld
```

## Dependencies

### All platforms

Install these tools and make sure they are in `PATH`:

```text
clang
clang++
ld.lld
python3 / python
qemu-system-aarch64
```

The compiler must support:

```text
--target=aarch64-none-elf
```

### Termux

Install the corresponding Clang, LLD, Python and QEMU packages available for your Termux setup.

### Linux

Use your distro package manager to install Clang/LLD, Python 3 and QEMU.

### macOS

Install LLVM, Python 3 and QEMU, then expose them through `PATH`.

### Windows

Install LLVM/Clang + LLD, Python 3 and QEMU. Run PowerShell as a normal user.

## Build

### Termux / Linux / macOS / WSL

```bash
chmod +x build.sh run.sh
./build.sh
```

Build and immediately boot:

```bash
./build.sh --run
```

Verbose build:

```bash
./build.sh --verbose
```

Clean generated compiler/test outputs:

```bash
./build.sh --clean
```

### Windows PowerShell

```powershell
.\build.ps1
```

Build and boot:

```powershell
.\build.ps1 --run
```

Verbose:

```powershell
.\build.ps1 --verbose
```

Clean generated compiler/test outputs:

```powershell
.\build.ps1 --clean
```

Run QEMU separately:

```powershell
.\run.ps1
```

## Output

The build keeps the same RioOS output locations:

```text
build/kernel.elf
storage/disk.img
```

`storage/disk.img` is preserved between builds. The build system never deletes the disk image automatically.

## Tool overrides

You can override tool names/paths with environment variables:

```bash
CLANG=/path/to/clang \
CXX=/path/to/clang++ \
LD=/path/to/ld.lld \
QEMU=/path/to/qemu-system-aarch64 \
./build.sh
```

Optional runner settings:

```bash
RIOOS_MEM=128M
RIOOS_CPU=cortex-a72
```

## Windows override example

```powershell
$env:CLANG="C:\LLVM\bin\clang.exe"
$env:CXX="C:\LLVM\bin\clang++.exe"
$env:LD="C:\LLVM\bin\ld.lld.exe"
$env:QEMU="C:\Program Files\qemu\qemu-system-aarch64.exe"
.\build.ps1
```

## Design goal

The kernel remains AArch64 bare-metal code. The host operating system only provides the build tools and QEMU.

The scripts intentionally separate:

```text
BUILD
  -> compile
  -> link
  -> host tests
  -> release verification

RUN
  -> QEMU virt
  -> GICv2
  -> Cortex-A72
  -> VirtIO block
  -> VirtIO network
```

This keeps RioOS portable without changing the kernel architecture.
