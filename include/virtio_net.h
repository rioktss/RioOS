#ifndef VIRTIO_NET_H
#define VIRTIO_NET_H

#include "types.h"

void virtio_net_init();
int virtio_net_present();
int virtio_net_ready();
void virtio_net_get_mac(uint8_t mac[6]);
int virtio_net_send(const void* frame, uint32_t length);
int virtio_net_receive(void* frame, uint32_t capacity);
uint64_t virtio_net_base();
int virtio_net_irq();
void virtio_net_status();

#endif
