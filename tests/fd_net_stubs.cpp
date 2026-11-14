/* Host-test stand-ins for the network stack calls made by kernel/fd.cpp (socket descriptors). */
#include <stdint.h>
int net_tcp_socket(int o) { return o >= 0 ? 1 : -22; }
int net_udp_socket(int o) { return o >= 0 ? 1 : -22; }
int net_tcp_close(int, int) { return 0; }
int net_udp_close(int, int) { return 0; }
int net_udp_bind(int, int, uint16_t) { return 0; }
int net_tcp_listen(int, int, uint16_t) { return 0; }
int net_tcp_accept(int, int, uint32_t*, uint16_t*, uint32_t) { return -11; }
int net_tcp_connect(int, int, uint32_t, uint16_t, uint32_t) { return -111; }
int net_tcp_send(int, int, const void*, uint32_t n, uint32_t) { return (int)n; }
int net_tcp_recv(int, int, void*, uint32_t, uint32_t) { return -110; }
int net_udp_sendto(int, int, uint32_t, uint16_t, const void*, uint32_t n) { return (int)n; }
int net_udp_recvfrom(int, int, uint32_t*, uint16_t*, void*, uint32_t, uint32_t) { return -110; }
int net_tcp_poll(int, int) { return 0; }
int net_udp_poll(int, int) { return 4; }
int net_tcp_shutdown_wr(int, int) { return 0; }
int net_tcp_info(int, int, uint32_t*, uint16_t*, uint16_t*) { return 0; }
int net_udp_local_port(int, int) { return 0; }
int net_poll_once() { return 0; }
int uart_try_getc(); int uart_has_data();

#include "tty.h"
int64_t tty_read(uint8_t* buf, uint64_t len) { uint64_t g = 0; while (g < len) { int c = uart_try_getc(); if (c < 0) break; buf[g++] = (uint8_t)c; } return (int64_t)g; }
int tty_poll() { return uart_has_data(); }
void tty_process_exit(int) {}
int tty_get(TtyTermios*) { return 0; }
int tty_set(int, const TtyTermios*, int) { return 0; }
void tty_winsize(uint16_t o[4]) { o[0] = 24; o[1] = 80; o[2] = o[3] = 0; }
uint64_t timer_millis() { static uint64_t t = 0; return t += 3; }

/* process / signal hooks used by signalfd, resource limits and EPOLLET counters */
uint32_t net_tcp_gen(int, int) { return 0; }
uint32_t net_udp_gen(int, int) { return 0; }
uint64_t process_rlimit_cur(int, int) { return ~0ULL; }
int process_sig_raise(int, int) { return 0; }
uint32_t process_sig_pending_raw(int) { return 0; }
uint32_t process_sig_serial(int) { return 0; }
int process_sig_take_set(int, uint32_t) { return 0; }
int process_sig_info_get(int, int, int*, int*, uint64_t*) { return 0; }
uint32_t process_uid(int) { return 0; }
