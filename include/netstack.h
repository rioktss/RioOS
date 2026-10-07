#ifndef NETSTACK_H
#define NETSTACK_H

#include "types.h"

void net_init();
int net_ready();
void net_status();
int net_parse_ipv4(const char* text, uint32_t* out);
void net_print_ipv4(uint32_t ip);
int net_poll_once();
int net_ping(uint32_t target_ip, uint32_t timeout_ms);

#endif
