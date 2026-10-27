# P3 Networking — RioOS 1.9.0

P3 preserves the existing VirtIO-Net + ARP/IPv4/ICMP path and adds bounded UDP/DNS support plus an outbound TCP client. The stack remains polling-based and deliberately does not enable timer IRQs.

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
- Four owner-scoped outbound TCP client sockets with SYN/SYN-ACK/ACK setup, TCP/IP checksums, bounded ACK/retransmission waits, a 4096-byte receive ring per socket, in-order data delivery, FIN/EOF handling, and process-exit cleanup.
- TCP syscalls 32–36 and `user_tcp_*` helpers for user ELFs.
- `httpget <hostname> [path]` resolves a name and fetches up to 4096 response bytes from a plain HTTP server on port 80. Existing `ifconfig`, `nslookup <hostname>`, root-only `dnsserver <ipv4>`, `ping`, and `net` commands remain.

## Shell examples

```text
net
ifconfig
ping 10.0.2.2
nslookup example.com
httpget neverssl.com /
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

TCP handles share a separate namespace from UDP handles and file descriptors; they are not valid arguments to `user_fd_read`/`user_fd_write`. The TCP API is a bounded teaching client API, not a complete POSIX sockets ABI.

Example TCP use from EL0:

```cpp
int64_t sock = user_tcp_socket();
if (sock >= 0 && user_tcp_connect(sock, 0x5DB8D822U, 80, 5000) == 0) {
    const char request[] = "GET / HTTP/1.0\r\nHost: example.com\r\nConnection: close\r\n\r\n";
    user_tcp_send(sock, request, sizeof(request) - 1, 5000);
    char response[1024];
    int64_t count = user_tcp_recv(sock, response, sizeof(response), 2500);
    user_tcp_close(sock);
}
```

## Validation

`tests/net_host_test.cpp` uses a fake VirtIO peer and covers IPv4 input parsing, ARP neighbor learning, UDP owner isolation/checksums, DNS A-record parsing, TCP three-way handshake, ACK-based sending, HTTP-like response delivery, EOF handling, owner isolation, and socket cleanup. `tests/verify_release.py` checks the network syscall and API wiring.

A successful host test and AArch64 link do not prove packet interoperability on a real QEMU host. The provided Linux/Termux runners already attach VirtIO-Net to QEMU user-mode networking (`10.0.2.15`, gateway `10.0.2.2`, DNS `10.0.2.3`) which permits outbound connections when the host has internet access. After starting the kernel, run `ping 10.0.2.2`, `nslookup example.com`, and `httpget neverssl.com /`.

## Deliberate limitations

- TCP is client-only. Passive listen/accept, concurrent TCP streams beyond the four bounded sockets, congestion control, TCP options/MSS negotiation, out-of-order reassembly, and robust close-state recovery are not implemented. Retransmission is bounded best-effort, not a full congestion-controlled TCP implementation.
- TLS/SSL and HTTPS are not implemented; `httpget` handles plain HTTP on port 80 only. Modern HTTPS-only sites will not return their content through this command.
- DNS AAAA/CNAME-follow-up behavior is not implemented.
- UDP is polling-based; it does not sleep a process or use an IRQ-driven receive path. A receive syscall can occupy the CPU until data arrives or its timeout expires.
- Static IPv4 settings only; no DHCP, route table, VLAN, IPv6, IP fragmentation/reassembly, multicast, or broadcast sockets.
- ARP cache expiry and comprehensive anti-spoofing policy are future work.
- The current process execution model remains synchronous with a shared EL0 virtual window; socket ownership prevents accidental cross-PID use but is not a substitute for full process isolation.
- Timer IRQ remains disabled during normal boot.
