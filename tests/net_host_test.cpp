#include "netstack.h"
#include <stdio.h>
#include <stdlib.h>

void uart_putc(char c){(void)c;}
void uart_puts(const char*s){(void)s;}
void virtio_net_init(){}
int virtio_net_present(){return 0;}
int virtio_net_ready(){return 0;}
void virtio_net_get_mac(unsigned char mac[6]){for(int i=0;i<6;i++)mac[i]=0;}
int virtio_net_send(const void*, unsigned){return -19;}
int virtio_net_receive(void*, unsigned){return 0;}
unsigned long long virtio_net_base(){return 0;}
int virtio_net_irq(){return -1;}
void virtio_net_status(){}
unsigned long long timer_millis(){return 0;}

static void fail(const char*m){fprintf(stderr,"NET TEST FAIL: %s\n",m);exit(1);}
int main(){
    uint32_t ip=0;
    if(!net_parse_ipv4("10.0.2.15",&ip)||ip!=((10U<<24)|(2U<<8)|15U))fail("valid ip");
    if(net_parse_ipv4("10.0.2.999",&ip))fail("invalid range");
    if(net_parse_ipv4("10.0.2",&ip))fail("short ip");
    if(net_parse_ipv4("10.0.2.15x",&ip))fail("trailing garbage");
    puts("NET HOST TEST: PASS");
    return 0;
}
