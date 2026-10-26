# RioOS Roadmap

RioOS is a long-term learning project. The roadmap is directional rather than a promise of specific release dates.

## Near-term goals

### Kernel stability

- harden interrupt and timer paths
- extend cooperative kernel threads toward validated preemption only after IRQ/input safety tests
- implement per-process page tables/address spaces and saved EL0 register frames before claiming process-level multitasking
- improve exception and fault diagnostics
- increase regression-test coverage

### Filesystem and storage

- strengthen VFS edge-case handling
- expand filesystem tests
- document on-disk structures
- improve recovery/error reporting

### Userspace

- complete pipe integration into the shell and arbitrary userspace process pipelines
- implement independent EL0 address spaces and saved process CPU contexts before enabling concurrent process execution
- add descriptor inheritance through `fork()` only after real process context switching is available
- expand the small userspace toolset
- improve the userspace API documentation
- make ELF loading diagnostics easier to understand

## P1 descriptor and pipe baseline (v1.7.0)

Implemented in this increment:

- descriptor tables keyed by process PID, with separate local descriptor numbers
- shared open-file descriptions and shared offsets for `dup`/`dup2`
- bounded non-blocking kernel pipes exposed through syscalls and userspace helpers
- host regression tests for PID isolation, duplicate descriptors, EOF, `EAGAIN`-style results, and `EPIPE`-style errors

Not implemented in this increment: real `fork()`/`wait()` or concurrent ELF processes. The current user runner enters one shared EL0 virtual window synchronously; adding process syscalls without independent address spaces and saved CPU contexts would falsely imply functionality and risk kernel integrity.


## P2 filesystem and permission baseline (v1.8.0)

Implemented in this increment:

- persistent UID/GID metadata stored in unused inode reserved bytes without changing the 128-byte inode layout
- conservative migration of legacy metadata: system-tree entries become root-owned, while entries inside `/home/user` and `/storage/home` become UID/GID 1000
- central owner/group/other permission checks for path traversal, file reads/writes, create/delete/rename, `chmod`, and `chown`
- `stat`, `chown`, and bounded `truncate` shell commands
- `SYS_FS_STAT`, `SYS_FS_TRUNCATE`, and `SYS_FS_CHMOD` plus userspace helpers
- executable permission required by the ELF loader; raw-disk reads/writes/cache flush require root
- regression tests for denied access, owner chmod, metadata persistence/migration, truncate zero-fill, and syscall credential restoration

The root password `123` is a development/demo credential embedded in source; it is not suitable for a security-sensitive deployment. This educational OS still has a single global shell role rather than separate per-process credential structures. Timer IRQ remains disabled during normal boot.

## Medium-term goals

### Hardware and drivers

- improve VirtIO robustness
- expand device support where useful to the educational goals
- make driver boundaries clearer

### Networking

- add TCP state machine, retransmission, connection teardown, and socket lifecycle tests
- add ARP cache expiry and safer neighbor update policy
- consider DHCP/configuration after UDP/DNS runtime validation
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


## P3 networking baseline (v1.9.0)

Implemented in this increment:

- bounded owner-PID-scoped UDP socket handles with bind, sendto, recvfrom, close, and per-process cleanup
- IPv4 header validation, fragment rejection (reassembly is not implemented), UDP length/checksum validation, and UDP transmit checksums
- receive queues are bounded; full queues drop newest datagrams and track drops internally; timeout is polling-based
- DNS A-record queries over UDP with QNAME validation, compressed-name bounds/loop handling, response ID/source checks, configurable DNS server, and QEMU's default `10.0.2.3` resolver
- UDP and DNS syscall wrappers for EL0 with userspace pointer validation
- shell commands `ifconfig`, `nslookup`, and root-only `dnsserver <ipv4>`
- host tests exercise ARP neighbor learning, malformed IPv4 rejection, owner isolation, UDP send/receive, and a fake DNS A response

Not implemented here: TCP, UDP socket handles are a dedicated networking handle namespace rather than ordinary file descriptors, DNS currently resolves A records only, DNS configuration is static (QEMU user-net default), IPv4 fragments are rejected, and runtime QEMU packet exchange must still be tested on a host with `qemu-system-aarch64`. No timer IRQ is enabled by default.


## P4 SMP, driver and framebuffer foundations (v1.10.0)

Implemented in this increment:

- CPU Affinity-0 reporting and an explicit one-online-CPU/SMP-disabled contract
- atomic acquire/release spinlock and try-lock primitives, documented as groundwork only
- framebuffer surface bounds handling, clearing of glyph background pixels, cursor clamping, and text-row scrolling
- host framebuffer tests using a caller-supplied RAM surface (no fixed physical address touched by host tests)
- VirtIO-Net RX descriptor length checks, no silent frame truncation, and buffer recycling for rejected but validly indexed packets
- `smp` and `fbinfo` shell diagnostics
- GitHub Actions CI for clean build, host regression tests, and release verification

Not implemented: secondary CPU release, per-CPU stacks/state, interrupt routing for multiple CPUs, system-wide SMP locking, general USB/HID input, a full desktop window manager, dynamic RPK execution, or timer preemption. The current QEMU pixel UI is a single-screen built-in launcher using ramfb plus a polled VirtIO keyboard/tablet path. Do not enable SMP by editing only the online CPU count; the boot and driver invariants must be completed together.


## Pixel UI foundation (v1.11.0)

- Added a QEMU `ramfb` XRGB8888 renderer configured through `fw_cfg` DMA.
- Added separate boot, lock screen, home screen, app router, and Terminal/Calculator/Clock/Files/Launcher modules under `ui/`.
- Added and image-packaged five `/apps/*.rpk` built-in-app manifests with release checks.
- Added `linux/run-ui.sh` and `windows/run-ui.ps1`; the established headless serial runners remain unchanged.
- QEMU-window keyboard and pointer input use VirtIO keyboard/tablet devices; UART remains a fallback. General USB/HID input, dynamic package execution, and the broader Bash/POSIX shell remain future work. Do not claim runtime stability until the graphical boot and navigation matrix has been tested on QEMU.
