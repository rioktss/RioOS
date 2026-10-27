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

/* DNS A-record lookup; IP addresses use the same 0xAABBCCDD representation
   as net_parse_ipv4(). The default DNS server is QEMU user-net (10.0.2.3). */
int net_dns_set_server(uint32_t server_ip);
uint32_t net_dns_server();
int net_dns_resolve(const char* hostname, uint32_t* out_ip, uint32_t timeout_ms);

#endif
