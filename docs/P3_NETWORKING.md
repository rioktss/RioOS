# P3 Networking — RioOS 1.9.0

P3 expands the existing VirtIO-Net + ARP/IPv4/ICMP code into a bounded UDP and DNS path. It deliberately does not enable timer IRQs and does not claim TCP support.

## Added capabilities

- UDP datagrams over the existing VirtIO-Net device and statically configured IPv4 interface (`10.0.2.15/24`, gateway `10.0.2.2`).
- Eight kernel-managed UDP socket handles. Handles are owned by a PID; another PID cannot bind, send, receive, or close that handle.
- Explicit bind, ephemeral ports (`bind(..., 0)`), send-to and receive-from operations.
- Four queued datagrams per socket, each with payload capped at 1200 bytes. Queue overflow drops the newest datagram; an oversized receive buffer returns an `EMSGSIZE`-style `-90` and consumes that datagram. Receive waits poll the NIC and are bounded by a timeout.
- IPv4 header checksum/length validation and explicit rejection of IPv4 fragments because reassembly is not implemented.
- UDP length validation, optional IPv4 UDP checksum acceptance on receive, transmit checksum generation, and per-process socket cleanup after EL0 returns.
- DNS A-record resolution over UDP. DNS message parsing bounds counts and offsets and protects compressed-name pointer traversal against out-of-range pointers and loops.
- DNS server configuration. The default is `10.0.2.3`, expected with QEMU user-mode networking.
- Syscalls `SYS_NET_UDP_SOCKET` through `SYS_NET_DNS_LOOKUP` (26–31) and `user_udp_*` / `user_dns_lookup` helpers.
- Shell commands `ifconfig`, `nslookup <hostname>`, and root-only `dnsserver <ipv4>`; `net` remains available.

## Shell examples

```text
net
ifconfig
ping 10.0.2.2
nslookup example.com
dnsserver 10.0.2.3
```

`dnsserver` changes the in-memory resolver address until the next `net_init()`; persistent network configuration and DHCP are not included. `nslookup` performs an A-record lookup only. A literal IPv4 address is reported as already numeric.

## EL0 UDP API

`include/user_api.h` defines:

```cpp
int64_t sock = user_udp_socket();
user_udp_bind(sock, 5000);                 // or port 0 for ephemeral
user_udp_sendto(sock, 0x0A000203U, 9999, payload, payload_len);
uint32_t peer_ip = 0;
uint16_t peer_port = 0;
user_udp_recvfrom(sock, &peer_ip, &peer_port, buffer, sizeof(buffer), 1000);
user_udp_close(sock);
```

The IPv4 address integer follows the `0xAABBCCDD` representation used by `net_parse_ipv4`. Return values are raw kernel results: a non-negative `sendto` result is bytes sent; receive returns bytes received; negative values are errno-like errors (`-19` unavailable, `-22` invalid input, `-24` resources exhausted, `-90` too large, `-98` port busy, `-110` timeout).

The socket handle namespace is separate from file descriptors in P3; handles are not valid arguments to `user_fd_read`/`user_fd_write`. This is a small teaching API, not a complete POSIX sockets ABI.

## Validation

`tests/net_host_test.cpp` uses a fake VirtIO peer and covers IPv4 input parsing, ARP neighbor learning, UDP owner isolation, port conflict, receive payload/source, rejection of corrupt IPv4 headers, transmit UDP checksum, DNS A-record parsing via a fake DNS responder, invalid hostnames, and socket cleanup. `tests/verify_release.py` checks the network syscall and API wiring.

A successful host test and AArch64 link do not prove packet interoperability in QEMU. Run `net`, `ping 10.0.2.2`, and `nslookup example.com` on a host with QEMU AArch64 and user-mode networking before treating runtime behavior as verified.

## Deliberate limitations

- TCP, retransmission, stream semantics, listen/connect/accept, and DNS AAAA/CNAME-follow-up behavior are not implemented.
- UDP is polling-based; it does not sleep a process or use an IRQ-driven receive path. A receive syscall can occupy the CPU until data arrives or its timeout expires.
- Static IPv4 settings only; no DHCP, route table, VLAN, IPv6, IP fragmentation/reassembly, multicast, or broadcast sockets.
- ARP cache expiry and comprehensive anti-spoofing policy are future work.
- The current process execution model remains synchronous with a shared EL0 virtual window; socket ownership prevents accidental cross-PID use but is not a substitute for full process isolation.
- Timer IRQ remains disabled during normal boot.
