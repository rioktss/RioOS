# RioOS 1.9.0 P3 test matrix

## Automated release checks

- AArch64 clean build with `-Wall -Wextra -Werror`, ELF link, and unresolved-symbol scan.
- Existing filesystem/VFS, memory, syscall, process/scheduler, FD/pipe, environment, diagnostics, and ELF static checks from P0-P2.
- Network host test with a fake VirtIO-Net device and synthetic ARP/IPv4/UDP frames.
- IPv4 parser rejects malformed/overflowing inputs.
- ARP reply populates the neighbor cache for outbound UDP resolution.
- UDP socket owner separation and duplicate-port protection.
- UDP receive delivers exact payload and peer address; empty queue times out.
- Invalid IPv4 header checksum rejected without queueing data.
- UDP transmission writes IPv4 and UDP lengths, payload, and checksum.
- DNS A query/response parses compression pointer and returns a known address.
- Invalid hostname and DNS-server address are rejected.
- Process socket cleanup releases all handles belonging to the terminating PID.
- Release verifier confirms syscall/API wiring and timer IRQ remains disabled during normal boot.

## Required on a real QEMU host

- `net` and `ifconfig` show interface ready, configured static address, gateway, DNS, and MAC.
- `ping 10.0.2.2` returns a reply.
- `nslookup example.com` returns an A record through QEMU's user-mode DNS resolver.
- Verify a user ELF can create/send/receive/close UDP via `include/user_api.h` without breaking shell keyboard input after return.

## Not covered / not implemented

TCP, DHCP, IPv6, packet fragmentation/reassembly, persistent resolver settings, concurrent process networking, and IRQ-driven receive are not covered by P3. The build environment used here does not provide QEMU runtime verification.
