# RioOS 1.9.0 P3 test matrix

> **v1.13.0 note:** the timer IRQ is now active (100 Hz tick, also while EL0 programs run) and the GUI was removed. Statements below that say the timer IRQ is disabled/off describe the earlier version this document was written for; see [TIMER_IRQ.md](./TIMER_IRQ.md) and [TERMINAL.md](./TERMINAL.md).


## Automated release checks

- AArch64 clean build with `-Wall -Wextra -Werror`, ELF link, and unresolved-symbol scan.
- Existing filesystem/VFS, memory, syscall, process/scheduler, FD/pipe, environment, diagnostics, and ELF static checks from P0-P2.
- Network host test with a fake VirtIO-Net device and synthetic ARP/IPv4/UDP/TCP frames.
- IPv4 parser rejects malformed/overflowing inputs.
- ARP reply populates the neighbor cache for outbound UDP resolution.
- UDP socket owner separation and duplicate-port protection.
- UDP receive delivers exact payload and peer address; empty queue times out.
- Invalid IPv4 header checksum rejected without queueing data.
- UDP transmission writes IPv4 and UDP lengths, payload, and checksum.
- DNS A query/response parses compression pointer and returns a known address.
- Invalid hostname and DNS-server address are rejected.
- TCP SYN/SYN-ACK/ACK handshake, checksum-valid ACK processing, bounded TCP send, receive-ring delivery, FIN/EOF, and cross-owner socket rejection.
- Process socket cleanup releases all UDP and TCP handles belonging to the terminating PID.
- Release verifier confirms syscall/API wiring and timer IRQ remains disabled during normal boot.

## Required on a real QEMU host

- `net` and `ifconfig` show interface ready, configured static address, gateway, DNS, and MAC.
- `ping 10.0.2.2` returns a reply.
- `nslookup example.com` returns an A record through QEMU's user-mode DNS resolver.
- `httpget neverssl.com /` establishes outbound TCP and prints an HTTP response over port 80.
- Verify a user ELF can create/send/receive/close UDP and create/connect/send/receive/close TCP via `include/user_api.h` without breaking shell keyboard input after return.

## Not covered / not implemented

TLS/HTTPS, TCP listen/accept, full TCP congestion control/reassembly, DHCP, IPv6, packet fragmentation/reassembly, persistent resolver settings, concurrent process networking, and IRQ-driven receive are not covered by P3. The build environment used here does not provide QEMU runtime verification.
