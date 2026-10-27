#include "netstack.h"
#include "virtio_net.h"
#include "timer.h"
#include "uart.h"
#include "string.h"

#define ETH_TYPE_ARP 0x0806U
#define ETH_TYPE_IPV4 0x0800U
#define ARP_HTYPE_ETH 1U
#define ARP_PTYPE_IPV4 0x0800U
#define ARP_OP_REQUEST 1U
#define ARP_OP_REPLY 2U
#define IP_PROTO_ICMP 1U
#define IP_PROTO_UDP 17U
#define IP_PROTO_TCP 6U
#define ICMP_ECHO_REPLY 0U
#define ICMP_ECHO_REQUEST 8U
#define NET_IP ((10U<<24)|(0U<<16)|(2U<<8)|15U)
#define NET_GATEWAY ((10U<<24)|(0U<<16)|(2U<<8)|2U)
#define NET_MASK 0xFFFFFF00U
#define NET_BROADCAST 0xFFFFFFFFU
#define MAX_FRAME 1600U
#define ARP_CACHE_SIZE 8
#define NET_ETH_HDR 14U
#define UDP_SOCKET_COUNT 8U
#define UDP_QUEUE_DEPTH 4U
#define UDP_EPHEMERAL_FIRST 49152U
#define UDP_EPHEMERAL_LAST 65535U
#define DNS_DEFAULT_SERVER ((10U<<24)|(0U<<16)|(2U<<8)|3U)
#define DNS_PORT 53U
#define UDP_HEADER_SIZE 8U
#define UDP_FRAME_MAX (sizeof(EthernetHeader) + sizeof(Ipv4Header) + UDP_HEADER_SIZE + NET_UDP_MAX_PAYLOAD)
#define TCP_SOCKET_COUNT 4U
#define TCP_RX_BUFFER_SIZE NET_TCP_MAX_IO
#define TCP_SEGMENT_MAX 536U
#define TCP_HEADER_SIZE 20U
#define TCP_RETRY_MS 500U
#define TCP_MAX_RETRIES 5U
#define TCP_FLAG_FIN 0x01U
#define TCP_FLAG_SYN 0x02U
#define TCP_FLAG_RST 0x04U
#define TCP_FLAG_PSH 0x08U
#define TCP_FLAG_ACK 0x10U
#define TCP_STATE_CLOSED 0U
#define TCP_STATE_SYN_SENT 1U
#define TCP_STATE_ESTABLISHED 2U
#define TCP_STATE_CLOSE_WAIT 3U
#define TCP_STATE_FIN_WAIT 4U
#define TCP_STATE_RESET 5U

struct __attribute__((packed)) EthernetHeader
{
    uint8_t dst[6];
    uint8_t src[6];
    uint16_t type;
};

struct __attribute__((packed)) ArpPacket
{
    uint16_t htype;
    uint16_t ptype;
    uint8_t hlen;
    uint8_t plen;
    uint16_t oper;
    uint8_t sha[6];
    uint32_t spa;
    uint8_t tha[6];
    uint32_t tpa;
};

struct __attribute__((packed)) Ipv4Header
{
    uint8_t ver_ihl;
    uint8_t tos;
    uint16_t total_len;
    uint16_t id;
    uint16_t frag;
    uint8_t ttl;
    uint8_t proto;
    uint16_t checksum;
    uint32_t src;
    uint32_t dst;
};

struct __attribute__((packed)) IcmpHeader
{
    uint8_t type;
    uint8_t code;
    uint16_t checksum;
    uint16_t id;
    uint16_t seq;
};

struct __attribute__((packed)) UdpHeader
{
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;
    uint16_t checksum;
};

struct TcpHeader
{
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq;
    uint32_t ack;
    uint8_t offset_reserved;
    uint8_t flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent;
} __attribute__((packed));

struct UdpDatagram
{
    uint32_t source_ip;
    uint16_t source_port;
    uint16_t length;
    uint8_t payload[NET_UDP_MAX_PAYLOAD];
};

struct UdpSocketState
{
    int used;
    int owner_pid;
    uint16_t local_port;
    uint16_t queue_head;
    uint16_t queue_count;
    uint32_t dropped;
    UdpDatagram queue[UDP_QUEUE_DEPTH];
};

struct TcpSocketState
{
    int used;
    int owner_pid;
    uint8_t state;
    uint8_t peer_closed;
    uint16_t local_port;
    uint16_t remote_port;
    uint32_t remote_ip;
    uint32_t snd_una;
    uint32_t snd_nxt;
    uint32_t rcv_nxt;
    uint16_t rx_head;
    uint16_t rx_count;
    uint8_t rx_buffer[TCP_RX_BUFFER_SIZE];
};

static int ready_state = 0;
static uint8_t local_mac[6];
static uint32_t next_ip_id = 1;

static struct
{
    int valid;
    uint32_t ip;
    uint8_t mac[6];
} arp_cache[ARP_CACHE_SIZE];

static uint8_t rx_frame[MAX_FRAME] __attribute__((aligned(16)));
static uint8_t tx_frame[MAX_FRAME] __attribute__((aligned(16)));
static UdpSocketState udp_sockets[UDP_SOCKET_COUNT];
static uint16_t udp_next_ephemeral = UDP_EPHEMERAL_FIRST;
static uint16_t tcp_next_ephemeral = UDP_EPHEMERAL_FIRST;
static TcpSocketState tcp_sockets[TCP_SOCKET_COUNT];
static uint16_t dns_query_id = 0x5249U;
static uint32_t dns_server_ip = DNS_DEFAULT_SERVER;

static int arp_resolve(uint32_t ip, uint8_t mac[6], uint32_t timeout_ms);
static void tcp_input(uint32_t source_ip, uint32_t dest_ip,
                      const uint8_t* bytes, uint32_t length);
static int tcp_send_segment(TcpSocketState* socket, uint8_t flags,
                            uint32_t sequence, uint32_t acknowledgement,
                            const uint8_t* payload, uint32_t payload_length);

// --- helpers be ---

static uint16_t read_be16(const void* p)
{
    const uint8_t* b = (const uint8_t*)p;
    return (uint16_t)(((uint16_t)b[0] << 8) | b[1]);
}

static void write_be16(void* p, uint16_t v)
{
    uint8_t* b = (uint8_t*)p;
    b[0] = (uint8_t)(v >> 8);
    b[1] = (uint8_t)v;
}

static uint32_t read_be32(const void* p)
{
    const uint8_t* b = (const uint8_t*)p;
    return ((uint32_t)b[0] << 24)
         | ((uint32_t)b[1] << 16)
         | ((uint32_t)b[2] << 8)
         | b[3];
}

static void write_be32(void* p, uint32_t v)
{
    uint8_t* b = (uint8_t*)p;
    b[0] = (uint8_t)(v >> 24);
    b[1] = (uint8_t)(v >> 16);
    b[2] = (uint8_t)(v >> 8);
    b[3] = (uint8_t)v;
}

static uint16_t checksum16(const uint8_t* data, uint32_t len)
{
    uint32_t sum = 0;
    uint32_t i = 0;

    while (i + 1 < len)
    {
        sum += ((uint16_t)data[i] << 8) | data[i + 1];
        i += 2;
        if (sum > 0xFFFFU)
        {
            sum = (sum & 0xFFFFU) + (sum >> 16);
        }
    }

    if (i < len)
    {
        sum += (uint16_t)data[i] << 8;
        if (sum > 0xFFFFU)
        {
            sum = (sum & 0xFFFFU) + (sum >> 16);
        }
    }

    while (sum >> 16)
    {
        sum = (sum & 0xFFFFU) + (sum >> 16);
    }

    return (uint16_t)~sum;
}

static void copy6(uint8_t* d, const uint8_t* s)
{
    for (int i = 0; i < 6; i++)
    {
        d[i] = s[i];
    }
}

static void fill6(uint8_t* d, uint8_t v)
{
    for (int i = 0; i < 6; i++)
    {
        d[i] = v;
    }
}

static int mac_is_valid_unicast(const uint8_t* mac)
{
    if (!mac || (mac[0] & 1U) != 0) return 0;
    uint8_t any = 0;
    for (int i = 0; i < 6; ++i) any |= mac[i];
    return any != 0;
}

static UdpSocketState* udp_get_socket(int owner_pid, int handle)
{
    if (owner_pid < 0 || handle < 1 || handle > (int)UDP_SOCKET_COUNT)
        return 0;
    UdpSocketState* socket = &udp_sockets[(uint32_t)(handle - 1)];
    if (!socket->used || socket->owner_pid != owner_pid)
        return 0;
    return socket;
}

static int udp_port_in_use(uint16_t port, const UdpSocketState* except)
{
    if (port == 0) return 0;
    for (uint32_t i = 0; i < UDP_SOCKET_COUNT; ++i)
    {
        const UdpSocketState* socket = &udp_sockets[i];
        if (socket->used && socket != except && socket->local_port == port)
            return 1;
    }
    return 0;
}

static uint16_t udp_checksum_ipv4(uint32_t source_ip, uint32_t dest_ip,
                                  const uint8_t* datagram, uint32_t length)
{
    /* Bounded temporary pseudo-header keeps checksum logic simple and avoids
       unaligned integer accesses on AArch64. */
    uint8_t bytes[12U + UDP_HEADER_SIZE + NET_UDP_MAX_PAYLOAD];
    if (!datagram || length < UDP_HEADER_SIZE ||
        length > UDP_HEADER_SIZE + NET_UDP_MAX_PAYLOAD)
        return 0;

    write_be32(bytes, source_ip);
    write_be32(bytes + 4, dest_ip);
    bytes[8] = 0;
    bytes[9] = IP_PROTO_UDP;
    write_be16(bytes + 10, (uint16_t)length);
    for (uint32_t i = 0; i < length; ++i)
        bytes[12U + i] = datagram[i];
    return checksum16(bytes, 12U + length);
}

static uint16_t tcp_checksum_ipv4(uint32_t source_ip, uint32_t dest_ip,
                                  const uint8_t* segment, uint32_t length)
{
    uint8_t bytes[12U + MAX_FRAME];
    if (!segment || length < TCP_HEADER_SIZE || length > MAX_FRAME - NET_ETH_HDR - sizeof(Ipv4Header))
        return 0;
    write_be32(bytes, source_ip);
    write_be32(bytes + 4U, dest_ip);
    bytes[8] = 0;
    bytes[9] = IP_PROTO_TCP;
    write_be16(bytes + 10U, (uint16_t)length);
    for (uint32_t i = 0; i < length; ++i) bytes[12U + i] = segment[i];
    return checksum16(bytes, 12U + length);
}

static int tcp_seq_after_eq(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) >= 0;
}

static TcpSocketState* tcp_get_socket(int owner_pid, int handle)
{
    if (owner_pid < 0 || handle < 1 || handle > (int)TCP_SOCKET_COUNT) return 0;
    TcpSocketState* socket = &tcp_sockets[(uint32_t)(handle - 1)];
    if (!socket->used || socket->owner_pid != owner_pid) return 0;
    return socket;
}

static int tcp_port_in_use(uint16_t port, const TcpSocketState* except)
{
    if (port == 0) return 0;
    for (uint32_t i = 0; i < TCP_SOCKET_COUNT; ++i)
        if (tcp_sockets[i].used && &tcp_sockets[i] != except && tcp_sockets[i].local_port == port)
            return 1;
    return 0;
}

static uint16_t tcp_receive_window(const TcpSocketState* socket)
{
    if (!socket || socket->rx_count >= TCP_RX_BUFFER_SIZE) return 0;
    return (uint16_t)(TCP_RX_BUFFER_SIZE - socket->rx_count);
}

static void udp_input(uint32_t source_ip, uint32_t dest_ip,
                      const uint8_t* bytes, uint32_t length)
{
    if (!bytes || length < UDP_HEADER_SIZE) return;
    const uint16_t source_port = read_be16(bytes);
    const uint16_t dest_port = read_be16(bytes + 2);
    const uint16_t udp_length = read_be16(bytes + 4);
    const uint16_t received_checksum = read_be16(bytes + 6);

    if (source_port == 0 || dest_port == 0 || udp_length < UDP_HEADER_SIZE ||
        udp_length != length || udp_length > UDP_HEADER_SIZE + NET_UDP_MAX_PAYLOAD)
        return;
    if (received_checksum != 0 &&
        udp_checksum_ipv4(source_ip, dest_ip, bytes, udp_length) != 0)
        return;

    for (uint32_t i = 0; i < UDP_SOCKET_COUNT; ++i)
    {
        UdpSocketState* socket = &udp_sockets[i];
        if (!socket->used || socket->local_port != dest_port)
            continue;
        if (socket->queue_count >= UDP_QUEUE_DEPTH)
        {
            if (socket->dropped != 0xFFFFFFFFU) ++socket->dropped;
            return; /* bounded queue: drop the newest datagram */
        }

        uint32_t qindex = (socket->queue_head + socket->queue_count) % UDP_QUEUE_DEPTH;
        UdpDatagram* packet = &socket->queue[qindex];
        packet->source_ip = source_ip;
        packet->source_port = source_port;
        packet->length = (uint16_t)(udp_length - UDP_HEADER_SIZE);
        for (uint32_t n = 0; n < packet->length; ++n)
            packet->payload[n] = bytes[UDP_HEADER_SIZE + n];
        ++socket->queue_count;
        return; /* ports are exclusive: exactly one socket receives a packet */
    }
}

// --- public api ---

void net_print_ipv4(uint32_t ip)
{
    uint8_t b[4] =
    {
        (uint8_t)(ip >> 24),
        (uint8_t)(ip >> 16),
        (uint8_t)(ip >> 8),
        (uint8_t)ip
    };

    for (int n = 0; n < 4; n++)
    {
        if (n) uart_putc('.');

        int v = b[n];

        if (v >= 100)
        {
            uart_putc((char)('0' + v / 100));
            v %= 100;
            uart_putc((char)('0' + v / 10));
            uart_putc((char)('0' + v % 10));
        }
        else if (v >= 10)
        {
            uart_putc((char)('0' + v / 10));
            uart_putc((char)('0' + v % 10));
        }
        else
        {
            uart_putc((char)('0' + v));
        }
    }
}

int net_parse_ipv4(const char* text, uint32_t* out)
{
    if (!text ||!out) return 0;

    uint32_t oct[4] = {0,0,0,0};
    int idx = 0;
    int digits = 0;

    while (1)
    {
        char c = *text++;

        if (c >= '0' && c <= '9')
        {
            if (oct[idx] > 25U) return 0;
            if (oct[idx] * 10U + (uint32_t)(c - '0') > 255U) return 0;

            oct[idx] = oct[idx] * 10U + (uint32_t)(c - '0');
            digits++;
            if (digits > 3) return 0;
            continue;
        }

        if (c == '.' && idx < 3 && digits)
        {
            idx++;
            digits = 0;
            continue;
        }

        if (c == '\0' && idx == 3 && digits) break;

        return 0;
    }

    *out = (oct[0] << 24) | (oct[1] << 16) | (oct[2] << 8) | oct[3];
    return 1;
}

static int arp_lookup(uint32_t ip, uint8_t mac[6])
{
    for (int i = 0; i < ARP_CACHE_SIZE; i++)
    {
        if (arp_cache[i].valid && arp_cache[i].ip == ip)
        {
            copy6(mac, arp_cache[i].mac);
            return 1;
        }
    }
    return 0;
}

static void arp_store(uint32_t ip, const uint8_t mac[6])
{
    int slot = -1;

    for (int i = 0; i < ARP_CACHE_SIZE; i++)
    {
        if (arp_cache[i].valid && arp_cache[i].ip == ip)
        {
            slot = i;
            break;
        }
    }

    if (slot < 0)
    {
        for (int i = 0; i < ARP_CACHE_SIZE; i++)
        {
            if (!arp_cache[i].valid)
            {
                slot = i;
                break;
            }
        }
    }

    if (slot < 0) slot = 0;

    arp_cache[slot].valid = 1;
    arp_cache[slot].ip = ip;
    copy6(arp_cache[slot].mac, mac);
}

static int send_arp(uint32_t target_ip)
{
    EthernetHeader* e = (EthernetHeader*)tx_frame;
    fill6(e->dst, 0xFF);
    copy6(e->src, local_mac);
    write_be16(&e->type, ETH_TYPE_ARP);

    ArpPacket* a = (ArpPacket*)(tx_frame + NET_ETH_HDR);
    write_be16(&a->htype, ARP_HTYPE_ETH);
    write_be16(&a->ptype, ARP_PTYPE_IPV4);
    a->hlen = 6;
    a->plen = 4;
    write_be16(&a->oper, ARP_OP_REQUEST);
    copy6(a->sha, local_mac);
    write_be32(&a->spa, NET_IP);
    fill6(a->tha, 0);
    write_be32(&a->tpa, target_ip);

    return virtio_net_send(tx_frame, NET_ETH_HDR + sizeof(ArpPacket));
}

static int send_arp_reply(const ArpPacket* req)
{
    EthernetHeader* e = (EthernetHeader*)tx_frame;
    copy6(e->dst, req->sha);
    copy6(e->src, local_mac);
    write_be16(&e->type, ETH_TYPE_ARP);

    ArpPacket* a = (ArpPacket*)(tx_frame + NET_ETH_HDR);
    write_be16(&a->htype, ARP_HTYPE_ETH);
    write_be16(&a->ptype, ARP_PTYPE_IPV4);
    a->hlen = 6;
    a->plen = 4;
    write_be16(&a->oper, ARP_OP_REPLY);
    copy6(a->sha, local_mac);
    write_be32(&a->spa, NET_IP);
    copy6(a->tha, req->sha);
    write_be32(&a->tpa, req->spa);

    return virtio_net_send(tx_frame, NET_ETH_HDR + sizeof(ArpPacket));
}

static int send_icmp_echo(uint32_t target, const uint8_t dst_mac[6], uint16_t ident, uint16_t seq)
{
    const char payload[] = "MyKernel";
    const uint32_t payload_len = 8;
    const uint32_t ip_len = sizeof(Ipv4Header) + sizeof(IcmpHeader) + payload_len;
    const uint32_t frame_len = NET_ETH_HDR + ip_len;

    EthernetHeader* e = (EthernetHeader*)tx_frame;
    copy6(e->dst, dst_mac);
    copy6(e->src, local_mac);
    write_be16(&e->type, ETH_TYPE_IPV4);

    Ipv4Header* ip = (Ipv4Header*)(tx_frame + NET_ETH_HDR);
    ip->ver_ihl = 0x45;
    ip->tos = 0;
    write_be16(&ip->total_len, (uint16_t)ip_len);
    write_be16(&ip->id, (uint16_t)next_ip_id++);
    write_be16(&ip->frag, 0);
    ip->ttl = 64;
    ip->proto = IP_PROTO_ICMP;
    write_be16(&ip->checksum, 0);
    write_be32(&ip->src, NET_IP);
    write_be32(&ip->dst, target);
    write_be16(&ip->checksum, checksum16((const uint8_t*)ip, sizeof(Ipv4Header)));

    IcmpHeader* ic = (IcmpHeader*)(tx_frame + NET_ETH_HDR + sizeof(Ipv4Header));
    ic->type = ICMP_ECHO_REQUEST;
    ic->code = 0;
    write_be16(&ic->checksum, 0);
    write_be16(&ic->id, ident);
    write_be16(&ic->seq, seq);

    for (uint32_t i = 0; i < payload_len; i++)
    {
        ((uint8_t*)ic)[sizeof(IcmpHeader) + i] = (uint8_t)payload[i];
    }

    write_be16(&ic->checksum, checksum16((const uint8_t*)ic, sizeof(IcmpHeader) + payload_len));

    return virtio_net_send(tx_frame, frame_len);
}

int net_poll_once()
{
    if (!ready_state) return -19;

    int len = virtio_net_receive(rx_frame, sizeof(rx_frame));
    if (len <= 0) return len;
    if ((uint32_t)len < sizeof(EthernetHeader)) return -5;

    EthernetHeader* e = (EthernetHeader*)rx_frame;
    uint16_t type = read_be16(&e->type);

    if (type == ETH_TYPE_ARP)
    {
        if ((uint32_t)len < NET_ETH_HDR + sizeof(ArpPacket)) return -5;

        ArpPacket* a = (ArpPacket*)(rx_frame + NET_ETH_HDR);

        if (read_be16(&a->htype) == ARP_HTYPE_ETH &&
            read_be16(&a->ptype) == ARP_PTYPE_IPV4 &&
            a->hlen == 6 && a->plen == 4)
        {
            uint32_t spa = read_be32(&a->spa);
            uint32_t tpa = read_be32(&a->tpa);
            uint16_t operation = read_be16(&a->oper);
            int ethernet_matches_arp = 1;
            for (int i = 0; i < 6; ++i)
                if (a->sha[i] != e->src[i]) ethernet_matches_arp = 0;
            int sender_ip_valid = spa != 0 && spa != NET_IP && spa != NET_BROADCAST &&
                                  (spa & 0xF0000000U) != 0xE0000000U;
            int sender_valid = sender_ip_valid && mac_is_valid_unicast(a->sha) &&
                               ethernet_matches_arp;

            if ((operation == ARP_OP_REQUEST || operation == ARP_OP_REPLY) && sender_valid)
                arp_store(spa, a->sha);

            if (operation == ARP_OP_REQUEST && tpa == NET_IP && sender_valid)
                send_arp_reply(a);
        }
        return 1;
    }

    if (type == ETH_TYPE_IPV4)
    {
        if ((uint32_t)len < NET_ETH_HDR + sizeof(Ipv4Header)) return -5;

        Ipv4Header* ip = (Ipv4Header*)(rx_frame + NET_ETH_HDR);
        uint8_t ihl = (uint8_t)((ip->ver_ihl & 0x0F) * 4U);

        if ((ip->ver_ihl >> 4) != 4) return -5;
        if (ihl < sizeof(Ipv4Header)) return -5;
        if ((uint32_t)ihl > (uint32_t)len - NET_ETH_HDR) return -5;
        uint16_t total = read_be16(&ip->total_len);
        if (total < ihl || (uint32_t)total > (uint32_t)len - NET_ETH_HDR) return -5;
        if (checksum16((const uint8_t*)ip, ihl) != 0) return -5;
        /* Fragment reassembly is deliberately unsupported; discard MF/offset
           packets instead of parsing a partial transport header as complete. */
        if ((read_be16(&ip->frag) & 0x3FFFU) != 0) return -5;
        uint32_t source_ip = read_be32(&ip->src);
        uint32_t dest_ip = read_be32(&ip->dst);
        if (dest_ip != NET_IP) return 1;
        if (ip->proto == IP_PROTO_UDP)
        {
            if (total < (uint16_t)(ihl + UDP_HEADER_SIZE)) return -5;
            udp_input(source_ip, dest_ip, (const uint8_t*)ip + ihl, total - ihl);
            return 1;
        }
        if (ip->proto == IP_PROTO_TCP)
        {
            if (total < (uint16_t)(ihl + TCP_HEADER_SIZE)) return -5;
            tcp_input(source_ip, dest_ip, (const uint8_t*)ip + ihl, total - ihl);
            return 1;
        }
        if (ip->proto != IP_PROTO_ICMP) return 1;
        if (total < (uint16_t)(ihl + sizeof(IcmpHeader))) return -5;

        IcmpHeader* ic = (IcmpHeader*)(rx_frame + NET_ETH_HDR + ihl);

        if (ic->type == ICMP_ECHO_REQUEST && ic->code == 0)
        {
            uint32_t icmp_len = total - ihl;
            uint8_t tmp[1500];
            if (icmp_len > sizeof(tmp)) return -5;

            for (uint32_t i = 0; i < icmp_len; i++)
            {
                tmp[i] = ((uint8_t*)ic)[i];
            }

            tmp[0] = ICMP_ECHO_REPLY;
            write_be16(tmp + 2, 0);
            write_be16(tmp + 2, checksum16(tmp, icmp_len));

            EthernetHeader* re = (EthernetHeader*)tx_frame;
            copy6(re->dst, e->src);
            copy6(re->src, local_mac);
            write_be16(&re->type, ETH_TYPE_IPV4);

            Ipv4Header* ri = (Ipv4Header*)(tx_frame + NET_ETH_HDR);

            for (uint32_t i = 0; i < ihl; i++)
            {
                ((uint8_t*)ri)[i] = ((uint8_t*)ip)[i];
            }

            write_be32(&ri->src, NET_IP);
            write_be32(&ri->dst, read_be32(&ip->src));
            write_be16(&ri->checksum, 0);
            write_be16(&ri->checksum, checksum16((uint8_t*)ri, ihl));

            for (uint32_t i = 0; i < icmp_len; i++)
            {
                ((uint8_t*)ri)[ihl + i] = tmp[i];
            }

            virtio_net_send(tx_frame, NET_ETH_HDR + total);
        }

        return 1;
    }

    return 1;
}

static int arp_resolve(uint32_t ip, uint8_t mac[6], uint32_t timeout_ms)
{
    if (arp_lookup(ip, mac)) return 0;
    if (send_arp(ip) < 0) return -1;

    uint64_t start = timer_millis();

    while (timer_millis() - start < timeout_ms)
    {
        net_poll_once();
        if (arp_lookup(ip, mac)) return 0;
    }

    return -1;
}

static int tcp_send_segment(TcpSocketState* socket, uint8_t flags,
                            uint32_t sequence, uint32_t acknowledgement,
                            const uint8_t* payload, uint32_t payload_length)
{
    if (!socket || !socket->used || socket->remote_ip == 0 || socket->remote_port == 0)
        return -22;
    if (payload_length > TCP_SEGMENT_MAX || (payload_length && !payload)) return -90;

    uint32_t next_hop = ((socket->remote_ip & NET_MASK) == (NET_IP & NET_MASK))
        ? socket->remote_ip : NET_GATEWAY;
    uint8_t target_mac[6];
    if (arp_resolve(next_hop, target_mac, 1000U) < 0) return -110;

    uint32_t tcp_length = TCP_HEADER_SIZE + payload_length;
    uint32_t ip_length = sizeof(Ipv4Header) + tcp_length;
    uint32_t frame_length = NET_ETH_HDR + ip_length;
    if (frame_length > sizeof(tx_frame)) return -90;

    EthernetHeader* ethernet = (EthernetHeader*)tx_frame;
    copy6(ethernet->dst, target_mac);
    copy6(ethernet->src, local_mac);
    write_be16(&ethernet->type, ETH_TYPE_IPV4);

    Ipv4Header* ip = (Ipv4Header*)(tx_frame + NET_ETH_HDR);
    ip->ver_ihl = 0x45;
    ip->tos = 0;
    write_be16(&ip->total_len, (uint16_t)ip_length);
    write_be16(&ip->id, (uint16_t)next_ip_id++);
    write_be16(&ip->frag, 0x4000U);
    ip->ttl = 64;
    ip->proto = IP_PROTO_TCP;
    write_be16(&ip->checksum, 0);
    write_be32(&ip->src, NET_IP);
    write_be32(&ip->dst, socket->remote_ip);
    write_be16(&ip->checksum, checksum16((const uint8_t*)ip, sizeof(Ipv4Header)));

    uint8_t* tcp = (uint8_t*)ip + sizeof(Ipv4Header);
    for (uint32_t i = 0; i < TCP_HEADER_SIZE; ++i) tcp[i] = 0;
    write_be16(tcp, socket->local_port);
    write_be16(tcp + 2U, socket->remote_port);
    write_be32(tcp + 4U, sequence);
    write_be32(tcp + 8U, acknowledgement);
    tcp[12] = 0x50U; /* 20-byte TCP header, no options */
    tcp[13] = flags;
    write_be16(tcp + 14U, tcp_receive_window(socket));
    write_be16(tcp + 16U, 0);
    write_be16(tcp + 18U, 0);
    for (uint32_t i = 0; i < payload_length; ++i) tcp[TCP_HEADER_SIZE + i] = payload[i];
    write_be16(tcp + 16U, tcp_checksum_ipv4(NET_IP, socket->remote_ip, tcp, tcp_length));

    return virtio_net_send(tx_frame, frame_length) == 0 ? 0 : -5;
}

static void tcp_input(uint32_t source_ip, uint32_t dest_ip,
                      const uint8_t* bytes, uint32_t length)
{
    if (!bytes || length < TCP_HEADER_SIZE || dest_ip != NET_IP) return;
    uint16_t source_port = read_be16(bytes);
    uint16_t dest_port = read_be16(bytes + 2U);
    uint32_t sequence = read_be32(bytes + 4U);
    uint32_t acknowledgement = read_be32(bytes + 8U);
    uint32_t header_length = (uint32_t)(bytes[12] >> 4) * 4U;
    uint8_t flags = bytes[13];
    if (source_port == 0 || dest_port == 0 || header_length < TCP_HEADER_SIZE ||
        header_length > length || tcp_checksum_ipv4(source_ip, dest_ip, bytes, length) != 0)
        return;

    TcpSocketState* socket = 0;
    for (uint32_t i = 0; i < TCP_SOCKET_COUNT; ++i)
    {
        TcpSocketState* candidate = &tcp_sockets[i];
        if (candidate->used && candidate->local_port == dest_port &&
            candidate->remote_ip == source_ip && candidate->remote_port == source_port &&
            candidate->state != TCP_STATE_CLOSED)
        {
            socket = candidate;
            break;
        }
    }
    if (!socket) return;

    if (flags & TCP_FLAG_RST)
    {
        socket->state = TCP_STATE_RESET;
        socket->peer_closed = 1;
        return;
    }

    if (socket->state == TCP_STATE_SYN_SENT)
    {
        if ((flags & (TCP_FLAG_SYN | TCP_FLAG_ACK)) == (TCP_FLAG_SYN | TCP_FLAG_ACK) &&
            acknowledgement == socket->snd_nxt)
        {
            socket->rcv_nxt = sequence + 1U;
            socket->snd_una = acknowledgement;
            socket->state = TCP_STATE_ESTABLISHED;
            (void)tcp_send_segment(socket, TCP_FLAG_ACK, socket->snd_nxt,
                                   socket->rcv_nxt, 0, 0);
        }
        return;
    }

    if (flags & TCP_FLAG_ACK)
    {
        if (tcp_seq_after_eq(acknowledgement, socket->snd_una) &&
            tcp_seq_after_eq(socket->snd_nxt, acknowledgement))
            socket->snd_una = acknowledgement;
    }

    uint32_t payload_length = length - header_length;
    int accepted_payload = 0;
    int should_ack = 0;
    if (payload_length != 0)
    {
        if (sequence == socket->rcv_nxt &&
            payload_length <= (uint32_t)(TCP_RX_BUFFER_SIZE - socket->rx_count))
        {
            uint32_t tail = (socket->rx_head + socket->rx_count) % TCP_RX_BUFFER_SIZE;
            for (uint32_t i = 0; i < payload_length; ++i)
                socket->rx_buffer[(tail + i) % TCP_RX_BUFFER_SIZE] = bytes[header_length + i];
            socket->rx_count = (uint16_t)(socket->rx_count + payload_length);
            socket->rcv_nxt += payload_length;
            accepted_payload = 1;
        }
        should_ack = 1; /* ACK duplicates/out-of-order data to request the expected byte. */
    }

    if (flags & TCP_FLAG_FIN)
    {
        uint32_t fin_position = sequence + payload_length;
        if ((accepted_payload || payload_length == 0) && fin_position == socket->rcv_nxt)
        {
            ++socket->rcv_nxt;
            socket->peer_closed = 1;
            if (socket->state == TCP_STATE_FIN_WAIT) socket->state = TCP_STATE_CLOSED;
            else if (socket->state == TCP_STATE_ESTABLISHED) socket->state = TCP_STATE_CLOSE_WAIT;
            should_ack = 1;
        }
    }

    if (should_ack)
        (void)tcp_send_segment(socket, TCP_FLAG_ACK, socket->snd_nxt,
                               socket->rcv_nxt, 0, 0);
}

int net_tcp_socket(int owner_pid)
{
    if (!ready_state) return -19;
    if (owner_pid < 0) return -22;
    for (uint32_t i = 0; i < TCP_SOCKET_COUNT; ++i)
    {
        TcpSocketState* socket = &tcp_sockets[i];
        if (socket->used) continue;
        socket->used = 1;
        socket->owner_pid = owner_pid;
        socket->state = TCP_STATE_CLOSED;
        socket->peer_closed = 0;
        socket->local_port = 0;
        socket->remote_port = 0;
        socket->remote_ip = 0;
        socket->snd_una = socket->snd_nxt = socket->rcv_nxt = 0;
        socket->rx_head = socket->rx_count = 0;
        return (int)i + 1;
    }
    return -24;
}

int net_tcp_connect(int owner_pid, int socket_handle, uint32_t target_ip,
                    uint16_t target_port, uint32_t timeout_ms)
{
    TcpSocketState* socket = tcp_get_socket(owner_pid, socket_handle);
    if (!socket) return -9;
    if (!ready_state) return -19;
    if (socket->state != TCP_STATE_CLOSED || target_ip == 0 || target_ip == NET_BROADCAST ||
        (target_ip & 0xF0000000U) == 0xE0000000U || target_port == 0 ||
        timeout_ms == 0 || timeout_ms > NET_TCP_MAX_TIMEOUT_MS)
        return -22;

    uint32_t attempts = (uint32_t)UDP_EPHEMERAL_LAST - UDP_EPHEMERAL_FIRST + 1U;
    while (attempts--)
    {
        uint16_t port = tcp_next_ephemeral;
        tcp_next_ephemeral = port == UDP_EPHEMERAL_LAST
            ? (uint16_t)UDP_EPHEMERAL_FIRST : (uint16_t)(port + 1U);
        if (!tcp_port_in_use(port, socket))
        {
            socket->local_port = port;
            break;
        }
    }
    if (socket->local_port == 0) return -98;

    socket->remote_ip = target_ip;
    socket->remote_port = target_port;
    uint32_t initial_sequence = (uint32_t)timer_millis() ^ target_ip ^
        ((uint32_t)socket->local_port << 16) ^ target_port;
    if (initial_sequence == 0) initial_sequence = 1;
    socket->snd_una = initial_sequence;
    socket->snd_nxt = initial_sequence + 1U;
    socket->rcv_nxt = 0;
    socket->state = TCP_STATE_SYN_SENT;

    int rc = tcp_send_segment(socket, TCP_FLAG_SYN, initial_sequence, 0, 0, 0);
    if (rc != 0)
    {
        socket->state = TCP_STATE_CLOSED;
        socket->local_port = 0;
        return rc;
    }

    uint64_t start = timer_millis();
    uint64_t last_send = start;
    uint32_t retries = 0;
    while (timer_millis() - start < timeout_ms)
    {
        (void)net_poll_once();
        if (socket->state == TCP_STATE_ESTABLISHED) return 0;
        if (socket->state == TCP_STATE_RESET)
        {
            socket->state = TCP_STATE_CLOSED;
            socket->local_port = 0;
            return -111;
        }
        uint64_t now = timer_millis();
        if (now - last_send >= TCP_RETRY_MS)
        {
            if (retries++ >= TCP_MAX_RETRIES) break;
            rc = tcp_send_segment(socket, TCP_FLAG_SYN, initial_sequence, 0, 0, 0);
            if (rc != 0) break;
            last_send = now;
        }
    }
    socket->state = TCP_STATE_CLOSED;
    socket->local_port = 0;
    socket->remote_ip = 0;
    socket->remote_port = 0;
    return -110;
}

int net_tcp_send(int owner_pid, int socket_handle, const void* data,
                 uint32_t length, uint32_t timeout_ms)
{
    TcpSocketState* socket = tcp_get_socket(owner_pid, socket_handle);
    if (!socket) return -9;
    if (!ready_state) return -19;
    if ((length && !data) || length > NET_TCP_MAX_IO ||
        timeout_ms == 0 || timeout_ms > NET_TCP_MAX_TIMEOUT_MS)
        return -22;
    if (socket->state != TCP_STATE_ESTABLISHED && socket->state != TCP_STATE_CLOSE_WAIT)
        return socket->state == TCP_STATE_RESET ? -104 : -107;
    if (length == 0) return 0;

    const uint8_t* input = (const uint8_t*)data;
    uint32_t sent_total = 0;
    uint64_t start = timer_millis();
    while (sent_total < length)
    {
        uint32_t chunk = length - sent_total;
        if (chunk > TCP_SEGMENT_MAX) chunk = TCP_SEGMENT_MAX;
        uint32_t sequence = socket->snd_nxt;
        uint32_t end_sequence = sequence + chunk;
        socket->snd_nxt = end_sequence;
        uint32_t retries = 0;
        uint64_t last_send = timer_millis();
        int rc = tcp_send_segment(socket, TCP_FLAG_ACK | TCP_FLAG_PSH, sequence,
                                  socket->rcv_nxt, input + sent_total, chunk);
        if (rc != 0)
        {
            socket->snd_nxt = sequence;
            return sent_total ? (int)sent_total : rc;
        }

        while (!tcp_seq_after_eq(socket->snd_una, end_sequence))
        {
            if (socket->state == TCP_STATE_RESET) return sent_total ? (int)sent_total : -104;
            if (socket->peer_closed && socket->state != TCP_STATE_CLOSE_WAIT)
                return sent_total ? (int)sent_total : -32;
            uint64_t now = timer_millis();
            if (now - start >= timeout_ms) return sent_total ? (int)sent_total : -110;
            if (now - last_send >= TCP_RETRY_MS)
            {
                if (retries++ >= TCP_MAX_RETRIES) return sent_total ? (int)sent_total : -110;
                rc = tcp_send_segment(socket, TCP_FLAG_ACK | TCP_FLAG_PSH, sequence,
                                     socket->rcv_nxt, input + sent_total, chunk);
                if (rc != 0) return sent_total ? (int)sent_total : rc;
                last_send = now;
            }
            (void)net_poll_once();
        }
        sent_total += chunk;
    }
    return (int)sent_total;
}

int net_tcp_recv(int owner_pid, int socket_handle, void* data,
                 uint32_t capacity, uint32_t timeout_ms)
{
    TcpSocketState* socket = tcp_get_socket(owner_pid, socket_handle);
    if (!socket) return -9;
    if (!ready_state) return -19;
    if (!data || capacity == 0 || capacity > NET_TCP_MAX_IO ||
        timeout_ms > NET_TCP_MAX_TIMEOUT_MS)
        return -22;

    uint64_t start = timer_millis();
    for (;;)
    {
        if (socket->rx_count != 0)
        {
            uint32_t copy_length = socket->rx_count;
            if (copy_length > capacity) copy_length = capacity;
            uint8_t* output = (uint8_t*)data;
            for (uint32_t i = 0; i < copy_length; ++i)
                output[i] = socket->rx_buffer[(socket->rx_head + i) % TCP_RX_BUFFER_SIZE];
            socket->rx_head = (uint16_t)((socket->rx_head + copy_length) % TCP_RX_BUFFER_SIZE);
            socket->rx_count = (uint16_t)(socket->rx_count - copy_length);
            if (socket->state == TCP_STATE_ESTABLISHED || socket->state == TCP_STATE_CLOSE_WAIT)
                (void)tcp_send_segment(socket, TCP_FLAG_ACK, socket->snd_nxt,
                                       socket->rcv_nxt, 0, 0);
            return (int)copy_length;
        }
        if (socket->state == TCP_STATE_RESET) return -104;
        if (socket->peer_closed || socket->state == TCP_STATE_CLOSED) return 0;
        if (socket->state != TCP_STATE_ESTABLISHED && socket->state != TCP_STATE_CLOSE_WAIT)
            return -107;
        if (timer_millis() - start >= timeout_ms) return -110;
        (void)net_poll_once();
    }
}

int net_tcp_close(int owner_pid, int socket_handle)
{
    TcpSocketState* socket = tcp_get_socket(owner_pid, socket_handle);
    if (!socket) return -9;
    int rc = 0;
    if (socket->state == TCP_STATE_ESTABLISHED || socket->state == TCP_STATE_CLOSE_WAIT)
    {
        uint32_t sequence = socket->snd_nxt;
        rc = tcp_send_segment(socket, TCP_FLAG_FIN | TCP_FLAG_ACK, sequence,
                              socket->rcv_nxt, 0, 0);
        if (rc == 0)
        {
            ++socket->snd_nxt;
            socket->state = TCP_STATE_FIN_WAIT;
            uint64_t start = timer_millis();
            while (!socket->peer_closed && timer_millis() - start < 250U)
                (void)net_poll_once();
        }
    }
    socket->used = 0;
    socket->owner_pid = -1;
    socket->state = TCP_STATE_CLOSED;
    socket->peer_closed = 0;
    socket->local_port = 0;
    socket->remote_port = 0;
    socket->remote_ip = 0;
    socket->rx_head = socket->rx_count = 0;
    return rc;
}

void net_tcp_close_all_for(int owner_pid)
{
    if (owner_pid < 0) return;
    for (uint32_t i = 0; i < TCP_SOCKET_COUNT; ++i)
    {
        TcpSocketState* socket = &tcp_sockets[i];
        if (!socket->used || socket->owner_pid != owner_pid) continue;
        if (socket->state == TCP_STATE_ESTABLISHED || socket->state == TCP_STATE_CLOSE_WAIT)
            (void)tcp_send_segment(socket, TCP_FLAG_FIN | TCP_FLAG_ACK, socket->snd_nxt,
                                   socket->rcv_nxt, 0, 0);
        socket->used = 0;
        socket->owner_pid = -1;
        socket->state = TCP_STATE_CLOSED;
        socket->peer_closed = 0;
        socket->local_port = 0;
        socket->remote_port = 0;
        socket->remote_ip = 0;
        socket->rx_head = socket->rx_count = 0;
    }
}

int net_udp_socket(int owner_pid)
{
    if (!ready_state) return -19;
    if (owner_pid < 0) return -22;
    for (uint32_t i = 0; i < UDP_SOCKET_COUNT; ++i)
    {
        UdpSocketState* socket = &udp_sockets[i];
        if (socket->used) continue;
        socket->used = 1;
        socket->owner_pid = owner_pid;
        socket->local_port = 0;
        socket->queue_head = 0;
        socket->queue_count = 0;
        socket->dropped = 0;
        return (int)i + 1;
    }
    return -24;
}

int net_udp_bind(int owner_pid, int socket_handle, uint16_t local_port)
{
    UdpSocketState* socket = udp_get_socket(owner_pid, socket_handle);
    if (!socket) return -9;
    if (!ready_state) return -19;
    if (socket->local_port != 0) return -22;

    if (local_port == 0)
    {
        uint32_t attempts = (uint32_t)UDP_EPHEMERAL_LAST - UDP_EPHEMERAL_FIRST + 1U;
        while (attempts--)
        {
            uint16_t candidate = udp_next_ephemeral;
            udp_next_ephemeral = candidate == UDP_EPHEMERAL_LAST
                ? (uint16_t)UDP_EPHEMERAL_FIRST : (uint16_t)(candidate + 1U);
            if (!udp_port_in_use(candidate, socket))
            {
                socket->local_port = candidate;
                return 0;
            }
        }
        return -98;
    }

    if (udp_port_in_use(local_port, socket)) return -98;
    socket->local_port = local_port;
    return 0;
}

int net_udp_sendto(int owner_pid, int socket_handle, uint32_t target_ip,
                   uint16_t target_port, const void* data, uint32_t length)
{
    UdpSocketState* socket = udp_get_socket(owner_pid, socket_handle);
    if (!socket) return -9;
    if (!ready_state) return -19;
    if (target_ip == 0 || target_ip == NET_BROADCAST ||
        (target_ip & 0xF0000000U) == 0xE0000000U || target_port == 0 ||
        length > NET_UDP_MAX_PAYLOAD || (length != 0 && data == 0))
        return -22;
    if (socket->local_port == 0)
    {
        int rc = net_udp_bind(owner_pid, socket_handle, 0);
        if (rc != 0) return rc;
    }

    uint32_t next_hop = ((target_ip & NET_MASK) == (NET_IP & NET_MASK))
        ? target_ip : NET_GATEWAY;
    uint8_t target_mac[6];
    if (arp_resolve(next_hop, target_mac, 1000U) < 0) return -110;

    const uint32_t udp_length = UDP_HEADER_SIZE + length;
    const uint32_t ip_length = sizeof(Ipv4Header) + udp_length;
    const uint32_t frame_length = NET_ETH_HDR + ip_length;
    if (frame_length > sizeof(tx_frame)) return -90;

    EthernetHeader* ethernet = (EthernetHeader*)tx_frame;
    copy6(ethernet->dst, target_mac);
    copy6(ethernet->src, local_mac);
    write_be16(&ethernet->type, ETH_TYPE_IPV4);

    Ipv4Header* ip = (Ipv4Header*)(tx_frame + NET_ETH_HDR);
    ip->ver_ihl = 0x45;
    ip->tos = 0;
    write_be16(&ip->total_len, (uint16_t)ip_length);
    write_be16(&ip->id, (uint16_t)next_ip_id++);
    write_be16(&ip->frag, 0x4000U); /* don't fragment */
    ip->ttl = 64;
    ip->proto = IP_PROTO_UDP;
    write_be16(&ip->checksum, 0);
    write_be32(&ip->src, NET_IP);
    write_be32(&ip->dst, target_ip);
    write_be16(&ip->checksum, checksum16((const uint8_t*)ip, sizeof(Ipv4Header)));

    UdpHeader* udp = (UdpHeader*)(tx_frame + NET_ETH_HDR + sizeof(Ipv4Header));
    write_be16(&udp->src_port, socket->local_port);
    write_be16(&udp->dst_port, target_port);
    write_be16(&udp->length, (uint16_t)udp_length);
    write_be16(&udp->checksum, 0);
    uint8_t* payload = (uint8_t*)udp + UDP_HEADER_SIZE;
    const uint8_t* source = (const uint8_t*)data;
    for (uint32_t i = 0; i < length; ++i) payload[i] = source[i];
    uint16_t checksum = udp_checksum_ipv4(NET_IP, target_ip, (const uint8_t*)udp, udp_length);
    if (checksum == 0) checksum = 0xFFFFU;
    write_be16(&udp->checksum, checksum);
    return virtio_net_send(tx_frame, frame_length) == 0 ? (int)length : -5;
}

int net_udp_recvfrom(int owner_pid, int socket_handle, uint32_t* source_ip,
                     uint16_t* source_port, void* data, uint32_t capacity,
                     uint32_t timeout_ms)
{
    UdpSocketState* socket = udp_get_socket(owner_pid, socket_handle);
    if (!socket) return -9;
    if (!ready_state) return -19;
    if (capacity == 0 || capacity > NET_UDP_MAX_PAYLOAD || data == 0 ||
        timeout_ms > NET_UDP_MAX_TIMEOUT_MS)
        return -22;

    uint64_t start = timer_millis();
    do
    {
        if (socket->queue_count != 0) break;
        (void)net_poll_once();
        if (socket->queue_count != 0) break;
    } while (timer_millis() - start < timeout_ms);

    if (socket->queue_count == 0) return -110;
    UdpDatagram* packet = &socket->queue[socket->queue_head];
    if (packet->length > capacity)
    {
        socket->queue_head = (uint16_t)((socket->queue_head + 1U) % UDP_QUEUE_DEPTH);
        --socket->queue_count;
        return -90; /* EMSGSIZE; the datagram is consumed, never partially copied */
    }

    uint8_t* output = (uint8_t*)data;
    for (uint32_t i = 0; i < packet->length; ++i) output[i] = packet->payload[i];
    if (source_ip) *source_ip = packet->source_ip;
    if (source_port) *source_port = packet->source_port;
    int result = (int)packet->length;
    socket->queue_head = (uint16_t)((socket->queue_head + 1U) % UDP_QUEUE_DEPTH);
    --socket->queue_count;
    return result;
}

int net_udp_close(int owner_pid, int socket_handle)
{
    UdpSocketState* socket = udp_get_socket(owner_pid, socket_handle);
    if (!socket) return -9;
    socket->used = 0;
    socket->owner_pid = -1;
    socket->local_port = 0;
    socket->queue_head = 0;
    socket->queue_count = 0;
    socket->dropped = 0;
    return 0;
}

void net_udp_close_all_for(int owner_pid)
{
    if (owner_pid < 0) return;
    for (uint32_t i = 0; i < UDP_SOCKET_COUNT; ++i)
    {
        if (udp_sockets[i].used && udp_sockets[i].owner_pid == owner_pid)
        {
            udp_sockets[i].used = 0;
            udp_sockets[i].owner_pid = -1;
            udp_sockets[i].local_port = 0;
            udp_sockets[i].queue_head = 0;
            udp_sockets[i].queue_count = 0;
            udp_sockets[i].dropped = 0;
        }
    }
}

static int dns_name_valid(const char* hostname, uint32_t* wire_length)
{
    if (!hostname) return 0;
    uint32_t total = 0;
    uint32_t label_length = 0;
    char previous = '.';
    for (uint32_t i = 0; hostname[i] != 0; ++i)
    {
        char c = hostname[i];
        if (i >= 254U) return 0;
        if (c == '.')
        {
            if (label_length == 0 || label_length > 63U || previous == '-') return 0;
            total += label_length + 1U;
            label_length = 0;
            previous = c;
            continue;
        }
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-')) return 0;
        if (label_length == 0 && c == '-') return 0;
        ++label_length;
        previous = c;
    }
    if (label_length == 0)
    {
        /* A final dot denotes the DNS root-relative form. */
        if (total == 0 || previous != '.') return 0;
        if (wire_length) *wire_length = total + 1U;
        return total + 1U <= 255U;
    }
    if (label_length > 63U || previous == '-') return 0;
    total += label_length + 2U; /* label length byte + label + root label */
    if (wire_length) *wire_length = total;
    return total <= 255U;
}

static int dns_skip_name(const uint8_t* message, uint32_t length, uint32_t* offset)
{
    if (!message || !offset || *offset >= length) return 0;
    uint32_t position = *offset;
    uint32_t consumed = 0;
    int jumped = 0;
    uint32_t steps = 0;
    while (position < length && steps++ <= length)
    {
        uint8_t label = message[position];
        if ((label & 0xC0U) == 0xC0U)
        {
            if (position + 1U >= length) return 0;
            uint32_t target = ((uint32_t)(label & 0x3FU) << 8) | message[position + 1U];
            if (target >= length) return 0;
            if (!jumped) consumed = position + 2U - *offset;
            jumped = 1;
            position = target;
            continue;
        }
        if ((label & 0xC0U) != 0) return 0;
        ++position;
        if (label == 0)
        {
            if (!jumped) consumed = position - *offset;
            *offset += consumed;
            return 1;
        }
        if (label > 63U || position + label > length) return 0;
        position += label;
    }
    return 0;
}

static int dns_parse_a_response(const uint8_t* message, uint32_t length,
                                uint16_t expected_id, uint32_t* out_ip)
{
    if (!message || !out_ip || length < 12U) return -5;
    if (read_be16(message) != expected_id) return -5;
    uint16_t flags = read_be16(message + 2);
    if ((flags & 0x8000U) == 0 || (flags & 0x0200U) != 0) return -5;
    uint16_t rcode = flags & 0x000FU;
    if (rcode == 3U) return -2; /* NXDOMAIN */
    if (rcode != 0U) return -5;
    uint16_t qdcount = read_be16(message + 4);
    uint16_t ancount = read_be16(message + 6);
    if (qdcount > 8U || ancount > 64U) return -5;

    uint32_t offset = 12U;
    for (uint16_t i = 0; i < qdcount; ++i)
    {
        if (!dns_skip_name(message, length, &offset) || offset + 4U > length) return -5;
        offset += 4U;
    }
    for (uint16_t i = 0; i < ancount; ++i)
    {
        if (!dns_skip_name(message, length, &offset) || offset + 10U > length) return -5;
        uint16_t type = read_be16(message + offset);
        uint16_t klass = read_be16(message + offset + 2U);
        uint16_t rdlength = read_be16(message + offset + 8U);
        offset += 10U;
        if (rdlength > length - offset) return -5;
        if (type == 1U && klass == 1U && rdlength == 4U)
        {
            uint32_t answer = read_be32(message + offset);
            if (answer != 0 && (answer & 0xF0000000U) != 0xE0000000U)
            {
                *out_ip = answer;
                return 0;
            }
        }
        offset += rdlength;
    }
    return -2; /* valid reply but no A answer */
}

int net_dns_set_server(uint32_t server_ip)
{
    if (server_ip == 0 || server_ip == NET_BROADCAST ||
        (server_ip & 0xF0000000U) == 0xE0000000U || server_ip == NET_IP)
        return -22;
    dns_server_ip = server_ip;
    return 0;
}

uint32_t net_dns_server()
{
    return dns_server_ip;
}

int net_dns_resolve(const char* hostname, uint32_t* out_ip, uint32_t timeout_ms)
{
    if (!ready_state) return -19;
    if (!hostname || !out_ip || timeout_ms == 0 || timeout_ms > NET_UDP_MAX_TIMEOUT_MS)
        return -22;

    uint32_t wire_length = 0;
    if (!dns_name_valid(hostname, &wire_length)) return -22;
    char normalized[254];
    uint32_t name_len = 0;
    while (hostname[name_len] && name_len < sizeof(normalized) - 1U)
    {
        normalized[name_len] = hostname[name_len];
        ++name_len;
    }
    if (name_len == 0 || name_len >= sizeof(normalized)) return -22;
    if (normalized[name_len - 1U] == '.') --name_len;
    normalized[name_len] = '\0';

    uint8_t query[512];
    for (uint32_t i = 0; i < sizeof(query); ++i) query[i] = 0;
    uint16_t query_id = ++dns_query_id;
    write_be16(query, query_id);
    write_be16(query + 2, 0x0100U); /* recursion desired */
    write_be16(query + 4, 1U);

    uint32_t query_length = 12U;
    uint32_t label_start = 0;
    for (uint32_t i = 0; i <= name_len; ++i)
    {
        if (i != name_len && normalized[i] != '.') continue;
        uint32_t label_length = i - label_start;
        if (label_length == 0 || label_length > 63U || query_length + label_length + 5U > sizeof(query))
            return -22;
        query[query_length++] = (uint8_t)label_length;
        for (uint32_t n = 0; n < label_length; ++n)
        {
            char c = normalized[label_start + n];
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            query[query_length++] = (uint8_t)c;
        }
        label_start = i + 1U;
    }
    query[query_length++] = 0;
    write_be16(query + query_length, 1U); query_length += 2U; /* A */
    write_be16(query + query_length, 1U); query_length += 2U; /* IN */
    (void)wire_length;

    int socket_handle = net_udp_socket(0);
    if (socket_handle < 0) return socket_handle;
    int rc = net_udp_bind(0, socket_handle, 0);
    if (rc == 0)
    {
        int sent = net_udp_sendto(0, socket_handle, dns_server_ip, DNS_PORT, query, query_length);
        rc = sent < 0 ? sent : 0;
    }

    uint32_t result_ip = 0;
    int resolved = 0;
    uint64_t start = timer_millis();
    while (rc == 0 && timer_millis() - start < timeout_ms)
    {
        uint64_t elapsed = timer_millis() - start;
        uint32_t remain = elapsed >= timeout_ms ? 0U : (uint32_t)(timeout_ms - elapsed);
        if (remain == 0) break;
        uint8_t response[NET_UDP_MAX_PAYLOAD];
        uint32_t source_ip = 0;
        uint16_t source_port = 0;
        int received = net_udp_recvfrom(0, socket_handle, &source_ip, &source_port,
                                        response, sizeof(response), remain);
        if (received < 0)
        {
            rc = received;
            break;
        }
        if (source_ip != dns_server_ip || source_port != DNS_PORT) continue;
        rc = dns_parse_a_response(response, (uint32_t)received, query_id, &result_ip);
        resolved = rc == 0;
        break;
    }
    if (rc == 0 && !resolved) rc = -110;
    if (rc == 0) *out_ip = result_ip;
    (void)net_udp_close(0, socket_handle);
    return rc;
}

int net_ping(uint32_t target_ip, uint32_t timeout_ms)
{
    if (!ready_state) return -19;

    uint32_t next_hop = ((target_ip & NET_MASK) == (NET_IP & NET_MASK))
                     ? target_ip
                      : NET_GATEWAY;

    uint8_t mac[6];
    if (arp_resolve(next_hop, mac, timeout_ms / 2) < 0) return -110;

    uint16_t ident = 0x4B4D;
    uint16_t seq = 1;

    if (send_icmp_echo(target_ip, mac, ident, seq) < 0) return -5;

    uint64_t start = timer_millis();

    while (timer_millis() - start < timeout_ms)
    {
        int n = virtio_net_receive(rx_frame, sizeof(rx_frame));

        if (n <= 0)
        {
            net_poll_once();
            continue;
        }

        if ((uint32_t)n < NET_ETH_HDR + sizeof(Ipv4Header)) continue;
        EthernetHeader* ethernet = (EthernetHeader*)rx_frame;
        if (read_be16(&ethernet->type) != ETH_TYPE_IPV4) continue;

        Ipv4Header* ip = (Ipv4Header*)(rx_frame + NET_ETH_HDR);
        if ((ip->ver_ihl >> 4) != 4 || ip->proto != IP_PROTO_ICMP) continue;
        uint8_t ihl = (uint8_t)((ip->ver_ihl & 0x0FU) * 4U);
        if (ihl < sizeof(Ipv4Header) || ihl > (uint32_t)n - NET_ETH_HDR) continue;
        uint16_t total = read_be16(&ip->total_len);
        if (total < (uint16_t)(ihl + sizeof(IcmpHeader)) ||
            total > (uint32_t)n - NET_ETH_HDR) continue;
        if (checksum16((const uint8_t*)ip, ihl) != 0) continue;
        if ((read_be16(&ip->frag) & 0x3FFFU) != 0) continue;
        if (read_be32(&ip->src) != target_ip || read_be32(&ip->dst) != NET_IP) continue;

        IcmpHeader* ic = (IcmpHeader*)((uint8_t*)ip + ihl);
        uint32_t icmp_length = total - ihl;
        if (checksum16((const uint8_t*)ic, icmp_length) != 0) continue;
        if (ic->type == ICMP_ECHO_REPLY && ic->code == 0 &&
            read_be16(&ic->id) == ident && read_be16(&ic->seq) == seq)
            return 0;
    }

    return -110;
}

void net_init()
{
    ready_state = 0;
    dns_server_ip = DNS_DEFAULT_SERVER;
    udp_next_ephemeral = UDP_EPHEMERAL_FIRST;
    for (uint32_t i = 0; i < UDP_SOCKET_COUNT; ++i)
    {
        udp_sockets[i].used = 0;
        udp_sockets[i].owner_pid = -1;
        udp_sockets[i].local_port = 0;
        udp_sockets[i].queue_head = 0;
        udp_sockets[i].queue_count = 0;
        udp_sockets[i].dropped = 0;
    }
    tcp_next_ephemeral = UDP_EPHEMERAL_FIRST;
    for (uint32_t i = 0; i < TCP_SOCKET_COUNT; ++i)
    {
        tcp_sockets[i].used = 0;
        tcp_sockets[i].owner_pid = -1;
        tcp_sockets[i].state = TCP_STATE_CLOSED;
        tcp_sockets[i].peer_closed = 0;
        tcp_sockets[i].local_port = 0;
        tcp_sockets[i].remote_port = 0;
        tcp_sockets[i].remote_ip = 0;
        tcp_sockets[i].snd_una = 0;
        tcp_sockets[i].snd_nxt = 0;
        tcp_sockets[i].rcv_nxt = 0;
        tcp_sockets[i].rx_head = 0;
        tcp_sockets[i].rx_count = 0;
    }
    for (int i = 0; i < ARP_CACHE_SIZE; i++)
    {
        arp_cache[i].valid = 0;
    }

    virtio_net_init();

    if (!virtio_net_ready())
    {
        uart_puts("Network stack: device unavailable.\r\n");
        return;
    }

    virtio_net_get_mac(local_mac);
    ready_state = 1;
    uart_puts("Network stack: IPv4 10.0.2.15/24 configured.\r\n");
}

int net_ready()
{
    return ready_state;
}

void net_status()
{
    uart_puts("Network status\r\n");
    uart_puts("---------------\r\n");
    uart_puts("Ready : ");
    uart_puts(ready_state? "yes\r\n" : "no\r\n");
    uart_puts("IPv4 : ");
    net_print_ipv4(NET_IP);
    uart_puts("/24\r\n");
    uart_puts("Gateway: ");
    net_print_ipv4(NET_GATEWAY);
    uart_puts("\r\nDNS    : ");
    net_print_ipv4(dns_server_ip);
    uart_puts("\r\nMAC : ");

    uint8_t m[6];
    virtio_net_get_mac(m);

    static const char h[] = "0123456789ABCDEF";

    for (int i = 0; i < 6; i++)
    {
        uart_putc(h[m[i] >> 4]);
        uart_putc(h[m[i] & 15]);
        if (i!= 5) uart_putc(':');
    }

    uart_puts("\r\n");
}