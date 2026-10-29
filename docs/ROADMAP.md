# RioOS Roadmap

RioOS is a long-term learning project. The roadmap is directional rather than a promise of specific release dates.

## P5 — Per-process userspace isolation (v1.12.0)

Implemented in this source baseline:

- PID-keyed EL0 address spaces with private page-table roots and private L3 tables for the executable image and stack ranges.
- Physical frames allocated for mapped ELF load segments and a private 64 KiB userspace stack; the lower stack region is left unmapped as a guard.
- ELF segment data is copied through kernel aliases into owned frames rather than writing to a shared fixed virtual image.
- Address-space activation/deactivation switches TTBR0_EL1 and invalidates stale translation state.
- PCB support to save and restore the 31 general-purpose registers, ELR, SPSR, and EL0 stack pointer at exception boundaries.
- User mappings and stack backing frames are released when the process finishes or loading fails.

Still deliberately deferred: scheduling a saved EL0 exception frame onto a different per-process kernel stack, safe process-to-process resume from IRQ/syscall exit, and timer preemption. The current `user_execute()` path remains synchronous; since 1.13.0 the timer IRQ stays live while EL0 runs but only counts ticks. P5 therefore provides real memory isolation and context capture, but it must not be described as full concurrent user-process multitasking yet.

## Near-term goals

### Kernel stability

- harden interrupt and timer paths
- extend cooperative kernel threads toward validated preemption only after IRQ/input safety tests
- implement scheduler-driven resume of saved EL0 exception frames on per-process kernel stacks before claiming process-level multitasking
- improve exception and fault diagnostics
- increase regression-test coverage

### Filesystem and storage

- strengthen VFS edge-case handling
- expand filesystem tests
- document on-disk structures
- improve recovery/error reporting

### Userspace

- complete pipe integration into the shell and arbitrary userspace process pipelines
- connect retained EL0 CPU contexts to a validated process scheduler and exception-return switch path
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
- shell commands `ifconfig`, `nslookup`, `httpget` (plain HTTP over outbound TCP), and root-only `dnsserver <ipv4>`
- outbound TCP connect/send/receive/close API and syscall wrappers for EL0
- host tests exercise ARP neighbor learning, malformed IPv4 rejection, UDP send/receive and DNS, TCP handshake/ACK/data/FIN, and owner isolation

Still limited: TCP is a bounded client only (no listen/accept, congestion control, options negotiation, or out-of-order reassembly); HTTPS/TLS is not implemented; UDP/TCP handles are separate from file descriptors; DNS resolves A records only; configuration is static (QEMU user-net default); IPv4 fragments are rejected; and real QEMU packet exchange must still be tested on a host with `qemu-system-aarch64`. No timer IRQ is enabled by default.


## P4 SMP, driver and framebuffer foundations (v1.10.0)

Implemented in this increment:

- CPU Affinity-0 reporting and an explicit one-online-CPU/SMP-disabled contract
- atomic acquire/release spinlock and try-lock primitives, documented as groundwork only
- framebuffer surface bounds handling, clearing of glyph background pixels, cursor clamping, and text-row scrolling
- host framebuffer tests using a caller-supplied RAM surface (no fixed physical address touched by host tests)
- VirtIO-Net RX descriptor length checks, no silent frame truncation, and buffer recycling for rejected but validly indexed packets
- `smp` and `fbinfo` shell diagnostics
- GitHub Actions CI for clean build, host regression tests, and release verification

Not implemented: secondary CPU release, per-CPU stacks/state, interrupt routing for multiple CPUs, system-wide SMP locking, general USB/HID input, a full desktop window manager, dynamic RPK execution, or timer preemption. Do not enable SMP by editing only the online CPU count; the boot and driver invariants must be completed together.


## Terminal window and live timer IRQ (v1.13.0)

- Removed the pixel UI (`ui/`, `apps/*.rpk`, `assets/`, `run-ui` scripts, UI docs and tests).
- Added a framebuffer text terminal (`drivers/console.cpp`) with DejaVu Sans Mono, Linux-console colours, ANSI handling, 2000-line scrollback and mouse-wheel / Shift+PageUp scrolling; `run-terminal` shows only the shell in the QEMU window.
- Enabled the 100 Hz timer IRQ at boot and while EL0 programs run (`SPSR 0x340`); it only counts ticks - preemptive process switching is still deferred.
- Still needs a runtime acceptance run on `qemu-system-aarch64`.

