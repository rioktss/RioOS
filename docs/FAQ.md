# RioOS FAQ

## Is RioOS a real operating system?

Yes. RioOS is a bare-metal operating-system project with its own kernel, boot path, memory management, process support, filesystem, system-call layer, userspace loading, and device/interrupt work. It is still a work in progress and is not intended to replace mature production operating systems.

## What is MyKernel?

**MyKernel** is the kernel inside the RioOS project. RioOS is the name of the overall operating system project; MyKernel is the kernel component.

## Is RioOS Linux-based?

No. The kernel is built as a freestanding AArch64 target and runs directly as the guest kernel in QEMU. The host machine only provides development tools and the emulator.

## Is it really bare metal if it runs in QEMU?

Yes, in the OS-development sense used by this project. RioOS runs without a host operating-system kernel underneath it inside the guest. QEMU provides the virtual machine hardware.

## Why ARM64?

ARM64/AArch64 is a modern architecture with a clear exception-level model and widely used virtualization/emulation support. QEMU also provides a convenient `virt` machine for experimentation.

## Can I run RioOS on a physical ARM64 board?

Not as a guaranteed drop-in target today. The repository is currently designed around QEMU's ARM64 `virt` machine and its virtual devices. Porting to real hardware would require hardware-specific work.

## Why C++ instead of only C?

RioOS is built with freestanding C++ and AArch64 Assembly. C++ features are used selectively; the project disables normal runtime features such as exceptions and RTTI and does not rely on a host C++ standard library.

## Where should I start reading the source?

Start with:

```text
boot/boot.S
linker.ld
kernel/kernel.cpp
```

Then continue through memory, interrupts, processes, filesystem, and userspace. [`docs/LEARN.md`](./LEARN.md) gives a suggested order.

## Can I learn OS development from RioOS?

Yes. The repository is intentionally shared as a learning and experimentation project. It is best used together with AArch64 documentation, QEMU documentation, and operating-system literature.

## Does RioOS guarantee every feature listed in the README is production-stable?

No. RioOS is explicitly a work in progress. Some subsystems are implemented and tested while others are actively being hardened or expanded.

## How can I contribute without knowing kernel programming yet?

Documentation, tests, diagrams, reproducibility notes, and bug reports are valuable contributions. You do not need to start by changing interrupt handlers or context-switch code.

## Who made RioOS?

RioOS is an open-source project by **Rio / rioktss**, developed in Indonesia and published publicly for the broader developer community.

## Where is the repository?

<https://github.com/rioktss/RioOS>
