# RioOS Roadmap

RioOS is a long-term learning project. The roadmap is directional rather than a promise of specific release dates.

## Near-term goals

### Kernel stability

- harden interrupt and timer paths
- make scheduler behaviour more deterministic
- improve exception and fault diagnostics
- increase regression-test coverage

### Filesystem and storage

- strengthen VFS edge-case handling
- expand filesystem tests
- document on-disk structures
- improve recovery/error reporting

### Userspace

- expand the small userspace toolset
- improve the userspace API documentation
- make ELF loading diagnostics easier to understand

## Medium-term goals

### Hardware and drivers

- improve VirtIO robustness
- expand device support where useful to the educational goals
- make driver boundaries clearer

### Networking

- extend the small network stack
- add more deterministic host/QEMU tests
- document the packet path from device to shell/API

### Tooling

- improve repeatable QEMU testing
- document reproducible development environments
- make build failures easier to diagnose

## Long-term direction

RioOS should become a compact but meaningful ARM64 teaching operating system where a new developer can go from:

```text
boot.S
  ↓
CPU + exceptions
  ↓
MMU + memory
  ↓
interrupts + timer
  ↓
processes + scheduler
  ↓
syscalls
  ↓
filesystem + devices
  ↓
ELF userspace
  ↓
shell and applications
```

and understand the complete path.

## What will not change

The project's educational character is important. RioOS does not need to become a giant kernel to be successful.

The preferred direction is:

- understandable code over unnecessary complexity
- reproducible builds over opaque tooling
- tests over guesswork
- documentation alongside implementation
- small, reviewable subsystems

## Community-driven roadmap

Ideas, bug reports, documentation improvements, tests, and thoughtful pull requests are welcome. The roadmap can change as contributors discover better ways to teach and implement the system.
