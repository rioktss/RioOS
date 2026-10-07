RioOS

RioOS is a simple ARM64 bare-metal operating system that uses MyKernel as its kernel and is designed to run on the QEMU ARM64 virt machine.

«🚧 Status: Still in development»

Features

- ARM64 kernel (MyKernel)
- UART & Shell
- Filesystem
- Persistent disk image
- Process management
- Scheduler
- System calls
- ELF userspace
- VirtIO Block
- VirtIO Network
- Generic Timer & GIC interrupt support
- MMU
- Host-side tests and release verification

---

Architecture

RioOS currently targets:

AArch64 / ARM64
        │
        ▼
QEMU virt machine
        │
        ├── GICv2
        ├── VirtIO Block
        ├── VirtIO Network
        └── Generic Timer
        │
        ▼
     MyKernel
        │
        ▼
      RioOS

RioOS is a bare-metal ARM64 operating system. Windows, Linux, macOS, and Termux are used as host environments for building and running RioOS through QEMU.

---

Requirements

You need the following tools:

- Clang
- Clang++
- LLD ("ld.lld")
- Python 3
- QEMU with "qemu-system-aarch64"

The compiler must support:

--target=aarch64-none-elf

---

Build System

RioOS provides a portable build system:

build.sh
build.ps1
run.sh
run.ps1

File| Platform
"build.sh"| Termux / Linux / macOS / WSL
"run.sh"| Termux / Linux / macOS / WSL
"build.ps1"| Windows PowerShell
"run.ps1"| Windows PowerShell

The build system separates building from running QEMU.

---

Termux / Android

1. Install dependencies

Install the required compiler, linker, Python and QEMU packages available for your Termux environment.

Then verify:

clang --version
clang++ --version
ld.lld --version
python3 --version
qemu-system-aarch64 --version

2. Clone RioOS

git clone https://github.com/rioktss/RioOS.git
cd RioOS

3. Make scripts executable

chmod +x build.sh run.sh

4. Build RioOS

./build.sh

A successful build produces:

build/kernel.elf
storage/disk.img

5. Run RioOS

./run.sh

Or build and run directly:

./build.sh --run

You should eventually see:

MyKernel initialization complete.
Welcome to MyKernel!
Type 'help' to get started.

[home@RioOS ]$

---

Arch Linux

1. Install dependencies

sudo pacman -Syu
sudo pacman -S clang lld python qemu-desktop

Check the installation:

clang --version
clang++ --version
ld.lld --version
python --version
qemu-system-aarch64 --version

2. Clone RioOS

git clone https://github.com/rioktss/RioOS.git
cd RioOS

3. Build

chmod +x build.sh run.sh
./build.sh

4. Run

./run.sh

Or:

./build.sh --run

---

Debian / Ubuntu / Other Linux

Install the equivalent packages for your distribution:

Clang
LLD
Python 3
QEMU

Then:

git clone https://github.com/rioktss/RioOS.git
cd RioOS
chmod +x build.sh run.sh
./build.sh
./run.sh

---

Windows

Windows uses PowerShell scripts.

Install:

- LLVM / Clang
- LLD
- Python 3
- QEMU

Make sure their "bin" directories are available in "PATH".

Open PowerShell in the RioOS directory:

cd RioOS

Build:

.\build.ps1

Run:

.\run.ps1

Or build and run:

.\build.ps1 --run

Verbose build:

.\build.ps1 --verbose

Clean generated compiler/test output:

.\build.ps1 --clean

---

Windows + WSL

RioOS can also be built inside WSL using the Linux workflow:

./build.sh

and:

./run.sh

QEMU availability and hardware acceleration depend on the WSL configuration.

---

macOS

Install:

- LLVM / Clang
- LLD
- Python 3
- QEMU

Then:

git clone https://github.com/rioktss/RioOS.git
cd RioOS
chmod +x build.sh run.sh
./build.sh
./run.sh

---

Build Options

Normal build

./build.sh

Builds the kernel and runs the host-side tests.

Verbose build

./build.sh --verbose

Shows compiler activity for each module.

Clean generated build output

./build.sh --clean

Removes generated object files and host test binaries.

«"storage/disk.img" is intentionally preserved.»

Build and run QEMU

./build.sh --run

---

QEMU Configuration

RioOS currently runs using:

Machine : QEMU virt
Architecture : ARM64 / AArch64
CPU : Cortex-A72
GIC : GICv2
Memory : 128 MB

The default QEMU command uses:

-kernel build/kernel.elf
-drive storage/disk.img
VirtIO Block
VirtIO Network

---

Build Output

After a successful build:

build/
├── kernel.elf
├── user_init.elf
└── *.o

storage/
└── disk.img

The main kernel image is:

build/kernel.elf

The persistent filesystem image is:

storage/disk.img

---

Tests

The build system automatically runs host-side tests:

FS HOST TEST       : PASS
NET HOST TEST      : PASS
MEM HOST TEST      : PASS
SYSCALL HOST TEST  : PASS
DIAG HOST TEST     : PASS
RELEASE STATIC CHECK: PASS

The build is considered successful only when compilation, linking, host tests, and release verification pass.

---

Running the Shell

After RioOS boots:

[home@RioOS ]$

Use:

help

to see the available commands.

Examples:

help
version
mem
ls
pwd
tree
ps
sched
uptime
elf
syscall ping
user
mmu
disk

Filesystem examples:

mkdir test
touch hello.txt
write hello.txt hello
cat hello.txt
ls

---

Portable Development

The host operating system does not change the target architecture.

                 Host OS
                    │
       ┌────────────┼────────────┐
       │            │            │
     Termux       Linux       Windows
       │            │            │
       └────────────┼────────────┘
                    ▼
                 QEMU
                    │
                    ▼
              ARM64 virt
                    │
                    ▼
                MyKernel
                    │
                    ▼
                  RioOS

This means developers can work on RioOS from different host operating systems while the kernel itself remains an ARM64 bare-metal kernel.

---

Important Note

RioOS currently runs inside QEMU.

It does not currently boot directly as a native operating system on arbitrary Android phones, PCs, or other hardware.

Native hardware support requires additional platform-specific work such as:

- Bootloader integration
- Hardware initialization
- UART drivers
- Interrupt controller support
- Timer support
- MMU/page table configuration
- Storage drivers
- Device-tree/platform support
- Board-specific drivers

The current primary target is:

ARM64 + QEMU virt + GICv2

---

Project Status

🚧 RioOS is still under active development.

The kernel, scheduler, interrupt subsystem, userspace, networking, filesystem, and hardware abstraction are continuing to evolve.

---

License

See ""LICENSE"" (LICENSE).