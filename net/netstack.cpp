#include "netstack.h"
#include "virtio_net.h"
#include "timer.h"
#include "uart.h"
#include "string.h"

#define ETH_TYPE_ARP  0x0806U
#define ETH_TYPE_IPV4 0x0800U
#define ARP_HTYPE_ETH 1U
#define ARP_PTYPE_IPV4 0x0800U
#define ARP_OP_REQUEST 1U
#define ARP_OP_REPLY   2U
#define IP_PROTO_ICMP  1U
#define ICMP_ECHO_REPLY 0U
#define ICMP_ECHO_REQUEST 8U
#define NET_IP ((10U<<24)|(0U<<16)|(2U<<8)|15U)
#define NET_GATEWAY ((10U<<24)|(0U<<16)|(2U<<8)|2U)
#define NET_MASK 0xFFFFFF00U
#define NET_BROADCAST 0xFFFFFFFFU
#define MAX_FRAME 1600U
#define ARP_CACHE_SIZE 8
#define NET_ETH_HDR 14U

struct __attribute__((packed)) EthernetHeader { uint8_t dst[6]; uint8_t src[6]; uint16_t type; };
struct __attribute__((packed)) ArpPacket { uint16_t htype; uint16_t ptype; uint8_t hlen; uint8_t plen; uint16_t oper; uint8_t sha[6]; uint32_t spa; uint8_t tha[6]; uint32_t tpa; };
struct __attribute__((packed)) Ipv4Header { uint8_t ver_ihl; uint8_t tos; uint16_t total_len; uint16_t id; uint16_t frag; uint8_t ttl; uint8_t proto; uint16_t checksum; uint32_t src; uint32_t dst; };
struct __attribute__((packed)) IcmpHeader { uint8_t type; uint8_t code; uint16_t checksum; uint16_t id; uint16_t seq; };

static int ready_state = 0;
static uint8_t local_mac[6];
static uint32_t next_ip_id = 1;
static struct { int valid; uint32_t ip; uint8_t mac[6]; } arp_cache[ARP_CACHE_SIZE];
static uint8_t rx_frame[MAX_FRAME] __attribute__((aligned(16)));
static uint8_t tx_frame[MAX_FRAME] __attribute__((aligned(16)));

static uint16_t read_be16(const void* p){const uint8_t*b=(const uint8_t*)p;return (uint16_t)(((uint16_t)b[0]<<8)|b[1]);}
static void write_be16(void* p,uint16_t v){uint8_t*b=(uint8_t*)p;b[0]=(uint8_t)(v>>8);b[1]=(uint8_t)v;}
static uint32_t read_be32(const void* p){const uint8_t*b=(const uint8_t*)p;return ((uint32_t)b[0]<<24)|((uint32_t)b[1]<<16)|((uint32_t)b[2]<<8)|b[3];}
static void write_be32(void* p,uint32_t v){uint8_t*b=(uint8_t*)p;b[0]=(uint8_t)(v>>24);b[1]=(uint8_t)(v>>16);b[2]=(uint8_t)(v>>8);b[3]=(uint8_t)v;}

static uint16_t checksum16(const uint8_t* data, uint32_t len)
{
    uint32_t sum=0; uint32_t i=0;
    while(i+1<len){sum += ((uint16_t)data[i]<<8)|data[i+1]; i+=2; if(sum>0xFFFFU)sum=(sum&0xFFFFU)+(sum>>16);}
    if(i<len){sum += (uint16_t)data[i]<<8; if(sum>0xFFFFU)sum=(sum&0xFFFFU)+(sum>>16);}
    while(sum>>16)sum=(sum&0xFFFFU)+(sum>>16);
    return (uint16_t)~sum;
}

static void copy6(uint8_t* d,const uint8_t*s){for(int i=0;i<6;i++)d[i]=s[i];}
static void fill6(uint8_t*d,uint8_t v){for(int i=0;i<6;i++)d[i]=v;}

void net_print_ipv4(uint32_t ip)
{
    uint8_t b[4]={(uint8_t)(ip>>24),(uint8_t)(ip>>16),(uint8_t)(ip>>8),(uint8_t)ip};
    for(int n=0;n<4;n++)
    {
        if(n) uart_putc('.');
        int v=b[n];
        if(v>=100){uart_putc((char)('0'+v/100));v%=100;uart_putc((char)('0'+v/10));uart_putc((char)('0'+v%10));}
        else if(v>=10){uart_putc((char)('0'+v/10));uart_putc((char)('0'+v%10));}
        else uart_putc((char)('0'+v));
    }
}

int net_parse_ipv4(const char* text,uint32_t*out)
{
    if(!text||!out)return 0; uint32_t oct[4]={0,0,0,0}; int idx=0,digits=0;
    while(1){char c=*text++; if(c>='0'&&c<='9'){if(oct[idx]>25U||oct[idx]*10U+(uint32_t)(c-'0')>255U)return 0;oct[idx]=oct[idx]*10U+(uint32_t)(c-'0');digits++;if(digits>3)return 0;continue;} if(c=='.'&&idx<3&&digits){idx++;digits=0;continue;} if(c=='\0'&&idx==3&&digits)break;return 0;}
    *out=(oct[0]<<24)|(oct[1]<<16)|(oct[2]<<8)|oct[3]; return 1;
}

static int arp_lookup(uint32_t ip,uint8_t mac[6])
{
    for(int i=0;i<ARP_CACHE_SIZE;i++)if(arp_cache[i].valid&&arp_cache[i].ip==ip){copy6(mac,arp_cache[i].mac);return 1;}return 0;
}
static void arp_store(uint32_t ip,const uint8_t mac[6])
{
    int slot=-1;for(int i=0;i<ARP_CACHE_SIZE;i++)if(arp_cache[i].valid&&arp_cache[i].ip==ip){slot=i;break;} if(slot<0)for(int i=0;i<ARP_CACHE_SIZE;i++)if(!arp_cache[i].valid){slot=i;break;} if(slot<0)slot=0;arp_cache[slot].valid=1;arp_cache[slot].ip=ip;copy6(arp_cache[slot].mac,mac);
}

static int send_arp(uint32_t target_ip)
{
    EthernetHeader* e=(EthernetHeader*)tx_frame;fill6(e->dst,0xFF);copy6(e->src,local_mac);write_be16(&e->type,ETH_TYPE_ARP);
    ArpPacket*a=(ArpPacket*)(tx_frame+NET_ETH_HDR);write_be16(&a->htype,ARP_HTYPE_ETH);write_be16(&a->ptype,ARP_PTYPE_IPV4);a->hlen=6;a->plen=4;write_be16(&a->oper,ARP_OP_REQUEST);copy6(a->sha,local_mac);write_be32(&a->spa,NET_IP);fill6(a->tha,0);write_be32(&a->tpa,target_ip);
    return virtio_net_send(tx_frame,NET_ETH_HDR+sizeof(ArpPacket));
}

static int send_arp_reply(const ArpPacket* req)
{
    EthernetHeader*e=(EthernetHeader*)tx_frame;copy6(e->dst,req->sha);copy6(e->src,local_mac);write_be16(&e->type,ETH_TYPE_ARP);ArpPacket*a=(ArpPacket*)(tx_frame+NET_ETH_HDR);write_be16(&a->htype,ARP_HTYPE_ETH);write_be16(&a->ptype,ARP_PTYPE_IPV4);a->hlen=6;a->plen=4;write_be16(&a->oper,ARP_OP_REPLY);copy6(a->sha,local_mac);write_be32(&a->spa,NET_IP);copy6(a->tha,req->sha);write_be32(&a->tpa,req->spa);return virtio_net_send(tx_frame,NET_ETH_HDR+sizeof(ArpPacket));
}

static int send_icmp_echo(uint32_t target,const uint8_t dst_mac[6],uint16_t ident,uint16_t seq)
{
    const char payload[]="MyKernel"; const uint32_t payload_len=8; const uint32_t ip_len=sizeof(Ipv4Header)+sizeof(IcmpHeader)+payload_len; const uint32_t frame_len=NET_ETH_HDR+ip_len;
    EthernetHeader*e=(EthernetHeader*)tx_frame;copy6(e->dst,dst_mac);copy6(e->src,local_mac);write_be16(&e->type,ETH_TYPE_IPV4);
    Ipv4Header*ip=(Ipv4Header*)(tx_frame+NET_ETH_HDR);ip->ver_ihl=0x45;ip->tos=0;write_be16(&ip->total_len,(uint16_t)ip_len);write_be16(&ip->id,(uint16_t)next_ip_id++);write_be16(&ip->frag,0);ip->ttl=64;ip->proto=IP_PROTO_ICMP;write_be16(&ip->checksum,0);write_be32(&ip->src,NET_IP);write_be32(&ip->dst,target);write_be16(&ip->checksum,checksum16((const uint8_t*)ip,sizeof(Ipv4Header)));
    IcmpHeader*ic=(IcmpHeader*)(tx_frame+NET_ETH_HDR+sizeof(Ipv4Header));ic->type=ICMP_ECHO_REQUEST;ic->code=0;write_be16(&ic->checksum,0);write_be16(&ic->id,ident);write_be16(&ic->seq,seq);for(uint32_t i=0;i<payload_len;i++)((uint8_t*)ic)[sizeof(IcmpHeader)+i]=(uint8_t)payload[i];write_be16(&ic->checksum,checksum16((const uint8_t*)ic,sizeof(IcmpHeader)+payload_len));return virtio_net_send(tx_frame,frame_len);
}

int net_poll_once()
{
    if(!ready_state)return -19;int len=virtio_net_receive(rx_frame,sizeof(rx_frame));if(len<=0)return len;if((uint32_t)len<sizeof(EthernetHeader))return -5;EthernetHeader*e=(EthernetHeader*)rx_frame;uint16_t type=read_be16(&e->type);
    if(type==ETH_TYPE_ARP){if((uint32_t)len<NET_ETH_HDR+sizeof(ArpPacket))return -5;ArpPacket*a=(ArpPacket*)(rx_frame+NET_ETH_HDR);if(read_be16(&a->htype)==ARP_HTYPE_ETH&&read_be16(&a->ptype)==ARP_PTYPE_IPV4&&a->hlen==6&&a->plen==4){uint32_t spa=read_be32(&a->spa);uint32_t tpa=read_be32(&a->tpa);arp_store(spa,a->sha);if(read_be16(&a->oper)==ARP_OP_REQUEST&&tpa==NET_IP)send_arp_reply(a);}return 1;}
    if(type==ETH_TYPE_IPV4){if((uint32_t)len<NET_ETH_HDR+sizeof(Ipv4Header))return -5;Ipv4Header*ip=(Ipv4Header*)(rx_frame+NET_ETH_HDR);uint8_t ihl=(uint8_t)((ip->ver_ihl&0x0F)*4U);if((ip->ver_ihl>>4)!=4||ihl<20||ihl>(uint8_t)((uint32_t)len-NET_ETH_HDR))return -5;if(read_be32(&ip->dst)!=NET_IP||ip->proto!=IP_PROTO_ICMP)return 1;uint16_t total=read_be16(&ip->total_len);if(total<ihl+sizeof(IcmpHeader)||total>(uint16_t)((uint32_t)len-NET_ETH_HDR))return -5;IcmpHeader*ic=(IcmpHeader*)(rx_frame+NET_ETH_HDR+ihl);if(ic->type==ICMP_ECHO_REQUEST&&ic->code==0){uint32_t icmp_len=total-ihl;uint8_t tmp[1500];if(icmp_len>sizeof(tmp))return -5;for(uint32_t i=0;i<icmp_len;i++)tmp[i]=((uint8_t*)ic)[i];tmp[0]=ICMP_ECHO_REPLY;write_be16(tmp+2,0);write_be16(tmp+2,checksum16(tmp,icmp_len));EthernetHeader*re=(EthernetHeader*)tx_frame;copy6(re->dst,e->src);copy6(re->src,local_mac);write_be16(&re->type,ETH_TYPE_IPV4);Ipv4Header*ri=(Ipv4Header*)(tx_frame+NET_ETH_HDR);for(uint32_t i=0;i<ihl;i++)((uint8_t*)ri)[i]=((uint8_t*)ip)[i];write_be32(&ri->src,NET_IP);write_be32(&ri->dst,read_be32(&ip->src));write_be16(&ri->checksum,0);write_be16(&ri->checksum,checksum16((uint8_t*)ri,ihl));for(uint32_t i=0;i<icmp_len;i++)((uint8_t*)ri)[ihl+i]=tmp[i];virtio_net_send(tx_frame,NET_ETH_HDR+total);}return 1;}
    return 1;
}

static int arp_resolve(uint32_t ip,uint8_t mac[6],uint32_t timeout_ms)
{
    if(arp_lookup(ip,mac))return 0;if(send_arp(ip)<0)return -1;uint64_t start=timer_millis();while(timer_millis()-start<timeout_ms){net_poll_once();if(arp_lookup(ip,mac))return 0;}return -1;
}

int net_ping(uint32_t target_ip,uint32_t timeout_ms)
{
    if(!ready_state)return -19;uint32_t next_hop=((target_ip&NET_MASK)==(NET_IP&NET_MASK))?target_ip:NET_GATEWAY;uint8_t mac[6];if(arp_resolve(next_hop,mac,timeout_ms/2)<0)return -110;uint16_t ident=0x4B4D,seq=1;if(send_icmp_echo(target_ip,mac,ident,seq)<0)return -5;uint64_t start=timer_millis();while(timer_millis()-start<timeout_ms){int n=virtio_net_receive(rx_frame,sizeof(rx_frame));if(n<=0){net_poll_once();continue;}if((uint32_t)n<NET_ETH_HDR+sizeof(Ipv4Header)+sizeof(IcmpHeader))continue;EthernetHeader*e=(EthernetHeader*)rx_frame;uint16_t type=read_be16(&e->type);if(type!=ETH_TYPE_IPV4)continue;Ipv4Header*ip=(Ipv4Header*)(rx_frame+NET_ETH_HDR);uint8_t ihl=(uint8_t)((ip->ver_ihl&0x0F)*4U);if((ip->ver_ihl>>4)!=4||ip->proto!=IP_PROTO_ICMP)continue;if(read_be32(&ip->src)!=target_ip||read_be32(&ip->dst)!=NET_IP)continue;IcmpHeader*ic=(IcmpHeader*)(rx_frame+NET_ETH_HDR+ihl);if(ic->type==ICMP_ECHO_REPLY&&read_be16(&ic->id)==ident&&read_be16(&ic->seq)==seq)return 0;}return -110;
}

void net_init()
{
    ready_state=0;for(int i=0;i<ARP_CACHE_SIZE;i++)arp_cache[i].valid=0;virtio_net_init();if(!virtio_net_ready()){uart_puts("Network stack: device unavailable.\r\n");return;}virtio_net_get_mac(local_mac);ready_state=1;uart_puts("Network stack: IPv4 10.0.2.15/24 configured.\r\n");}
int net_ready(){return ready_state;}
void net_status(){uart_puts("Network status\r\n---------------\r\nReady : ");uart_puts(ready_state?"yes\r\n":"no\r\n");uart_puts("IPv4  : ");net_print_ipv4(NET_IP);uart_puts("/24\r\nGateway: ");net_print_ipv4(NET_GATEWAY);uart_puts("\r\nMAC   : ");uint8_t m[6];virtio_net_get_mac(m);static const char h[]="0123456789ABCDEF";for(int i=0;i<6;i++){uart_putc(h[m[i]>>4]);uart_putc(h[m[i]&15]);if(i!=5)uart_putc(':');}uart_puts("\r\n");}
