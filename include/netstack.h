/*
 * MIT License
 *
 * Copyright (c) 2026 Rio okta kusuma
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files, to deal in the Software
 * without restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies of the
 * Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef NETSTACK_H
#define NETSTACK_H

#include "types.h"

#define NET_UDP_MAX_PAYLOAD 1200U
#define NET_UDP_MAX_TIMEOUT_MS 10000U
#define NET_TCP_MAX_TIMEOUT_MS 10000U
#define NET_TCP_MAX_IO 4096U

void net_init();
int net_ready();
void net_status();
uint32_t net_local_ip();
int net_parse_ipv4(const char* text, uint32_t* out);
void net_print_ipv4(uint32_t ip);
int net_poll_once();
int net_ping(uint32_t target_ip, uint32_t timeout_ms);

/* Small, bounded UDP socket API. Handles are kernel-managed and are scoped
   to the owning PID; port zero requests an ephemeral port. Calls are polling
   based because timer IRQs remain disabled in the default boot path. */
int net_udp_socket(int owner_pid);
int net_udp_bind(int owner_pid, int socket_handle, uint16_t local_port);
int net_udp_sendto(int owner_pid, int socket_handle, uint32_t target_ip,
                   uint16_t target_port, const void* data, uint32_t length);
int net_udp_recvfrom(int owner_pid, int socket_handle, uint32_t* source_ip,
                     uint16_t* source_port, void* data, uint32_t capacity,
                     uint32_t timeout_ms);
int net_udp_close(int owner_pid, int socket_handle);
void net_udp_close_all_for(int owner_pid);

/* Bounded outbound TCP client API. TCP is polling-based and targets ordinary
   IPv4 services such as HTTP; TLS/HTTPS is not implemented by this stack. */
int net_tcp_socket(int owner_pid);
int net_tcp_connect(int owner_pid, int socket_handle, uint32_t target_ip,
                    uint16_t target_port, uint32_t timeout_ms);
int net_tcp_send(int owner_pid, int socket_handle, const void* data,
                 uint32_t length, uint32_t timeout_ms);
int net_tcp_recv(int owner_pid, int socket_handle, void* data,
                 uint32_t capacity, uint32_t timeout_ms);
int net_tcp_close(int owner_pid, int socket_handle);
void net_tcp_close_all_for(int owner_pid);
int net_tcp_poll(int owner_pid, int socket_handle);   /* Linux poll bits: IN 1, OUT 4, ERR 8, HUP 16, NVAL 32 */
int net_udp_poll(int owner_pid, int socket_handle);
int net_tcp_shutdown_wr(int owner_pid, int socket_handle);
int net_tcp_info(int owner_pid, int socket_handle, uint32_t* remote_ip, uint16_t* remote_port, uint16_t* local_port);
int net_udp_local_port(int owner_pid, int socket_handle);

/* TCP server: listen on a local port, accept returns a connected handle (same send/recv/close API). */
int net_tcp_listen(int owner_pid, int socket_handle, uint16_t local_port);
int net_http_get_url(const char* url, int insecure, const char* cacert, uint8_t* out, uint32_t cap);   /* HTTP(S) GET, body length or -errno (kernel/commands.cpp) */
int net_tcp_accept(int owner_pid, int socket_handle, uint32_t* peer_ip, uint16_t* peer_port, uint32_t timeout_ms);

/* DHCP client (DISCOVER/OFFER/REQUEST/ACK): applies address, mask, router and DNS on success. */
int net_dhcp(uint32_t timeout_ms);
int net_dhcp_renew(uint32_t timeout_ms);
int net_dhcp_release();
uint32_t net_dhcp_remaining();
void net_dhcp_maintain();
void net_dhcp_info(uint32_t* server, uint32_t* lease_seconds, int* bound);

/* IPv6 link-local (fe80::/64 from the MAC), NDP and ICMPv6 echo. */
int net_ip6_parse(const char* text, uint8_t out[16]);
void net_ip6_print(const uint8_t addr[16]);
void net_ip6_local(uint8_t out[16]);
int net_ping6(const uint8_t target[16], uint32_t timeout_ms);   /* RTT ms or negative errno */
void net_ip6_stats(uint32_t* ns_received, uint32_t* na_sent, uint32_t* echo_requests);

/* UDP/TCP over IPv6 link-local (neighbours resolved with NDP). Sockets come from net_udp_socket()/net_tcp_socket();
   a TCP listener accepts both IPv4 and IPv6 clients (net_tcp_peer6 tells which). */
int net_udp6_sendto(int owner_pid, int socket_handle, const uint8_t target[16], uint16_t target_port, const void* data, uint32_t length);
int net_udp6_recvfrom(int owner_pid, int socket_handle, uint8_t source[16], uint16_t* source_port, void* data, uint32_t capacity, uint32_t timeout_ms);
int net_tcp6_connect(int owner_pid, int socket_handle, const uint8_t target[16], uint16_t target_port, uint32_t timeout_ms);
int net_tcp_peer6(int owner_pid, int socket_handle, uint8_t out[16]);

/* DNS A-record lookup; IP addresses use the same 0xAABBCCDD representation
   as net_parse_ipv4(). The default DNS server is QEMU user-net (10.0.2.3). */
int net_dns_set_server(uint32_t server_ip);
uint32_t net_dns_server();
int net_dns_resolve(const char* hostname, uint32_t* out_ip, uint32_t timeout_ms);


/* packet filter (IPv4). Rules are ordered, first match wins; unmatched traffic follows the per-direction default.
   Inbound rules apply to new connections only; DHCP/DNS are exempt. */
struct NetFwRule { uint8_t used; uint8_t allow; uint8_t dir; uint8_t proto; uint32_t ip; uint8_t prefix; uint16_t port; };   /* dir 0 in, 1 out, 2 both; proto 0 any 1 icmp 6 tcp 17 udp */
int net_fw_add(const NetFwRule* rule, int position);      /* index or -errno; position -1 appends */
int net_fw_del(int index);
void net_fw_flush();
int net_fw_get(int index, NetFwRule* out);
void net_fw_set_default(int dir, int allow);
void net_fw_get_state(int* def_in, int* def_out, uint32_t* drop_in, uint32_t* drop_out);

#endif
