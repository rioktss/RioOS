#include "netstack.h"
#include "virtio_net.h"
#include "timer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint32_t LOCAL_IP = 0x0A00020FU;
static const uint32_t DNS_IP = 0x0A000203U;
static const uint32_t ANSWER_IP = 0x5DB8D822U;
static const uint8_t LOCAL_MAC[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
static const uint8_t DNS_MAC[6] = {0x52, 0x55, 0x0A, 0x00, 0x02, 0x03};
static uint64_t fake_ms = 0;
static uint8_t rx_queue[16][1600];
static uint32_t rx_lengths[16];
static unsigned rx_head = 0, rx_count = 0;
static uint8_t last_tx[1600];
static uint32_t last_tx_length = 0;
static int fake_present = 1, fake_ready = 1;

static void fail(const char* message)
{
    fprintf(stderr, "NET TEST FAIL: %s\n", message);
    exit(1);
}
#define CHECK(c, message) do { if (!(c)) fail(message); } while (0)

static uint16_t be16(const uint8_t* p) { return (uint16_t)(((uint16_t)p[0] << 8) | p[1]); }
static void put16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static uint16_t checksum(const uint8_t* data, uint32_t length);
static void put32(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static uint16_t udp_checksum_value(uint32_t source_ip, uint32_t dest_ip,
                                   const uint8_t* udp, uint32_t length)
{
    uint8_t pseudo[12U + 8U + NET_UDP_MAX_PAYLOAD];
    if (!udp || length < 8U || length > 8U + NET_UDP_MAX_PAYLOAD) return 0;
    put32(pseudo, source_ip); put32(pseudo + 4, dest_ip);
    pseudo[8] = 0; pseudo[9] = 17; put16(pseudo + 10, (uint16_t)length);
    memcpy(pseudo + 12, udp, length);
    return checksum(pseudo, 12U + length);
}
static int udp_checksum_valid(uint32_t source_ip, uint32_t dest_ip,
                              const uint8_t* udp, uint32_t length)
{
    uint8_t pseudo[12U + 8U + NET_UDP_MAX_PAYLOAD];
    if (!udp || length < 8U || length > 8U + NET_UDP_MAX_PAYLOAD) return 0;
    put32(pseudo, source_ip); put32(pseudo + 4, dest_ip);
    pseudo[8] = 0; pseudo[9] = 17; put16(pseudo + 10, (uint16_t)length);
    memcpy(pseudo + 12, udp, length);
    return checksum(pseudo, 12U + length) == 0;
}
static uint16_t checksum(const uint8_t* data, uint32_t length)
{
    uint32_t sum = 0, i = 0;
    while (i + 1U < length)
    {
        sum += ((uint16_t)data[i] << 8) | data[i + 1U]; i += 2;
        if (sum > 0xFFFFU) sum = (sum & 0xFFFFU) + (sum >> 16);
    }
    if (i < length) sum += (uint16_t)data[i] << 8;
    while (sum >> 16) sum = (sum & 0xFFFFU) + (sum >> 16);
    return (uint16_t)~sum;
}
static int enqueue_frame(const uint8_t* frame, uint32_t length)
{
    if (!frame || length == 0 || length > sizeof(rx_queue[0]) || rx_count >= 16U) return 0;
    unsigned slot = (rx_head + rx_count) % 16U;
    memcpy(rx_queue[slot], frame, length);
    rx_lengths[slot] = length;
    ++rx_count;
    return 1;
}
static void make_ipv4_udp_frame(uint8_t* out, uint32_t* out_len,
                                uint32_t source_ip, uint16_t source_port,
                                uint32_t target_ip, uint16_t target_port,
                                const uint8_t* payload, uint32_t payload_len,
                                int bad_ip_checksum)
{
    memset(out, 0, 1600);
    memcpy(out, LOCAL_MAC, 6); /* Ethernet destination: local interface */
    memcpy(out + 6, DNS_MAC, 6);
    put16(out + 12, 0x0800U);
    uint8_t* ip = out + 14;
    uint16_t ip_len = (uint16_t)(20U + 8U + payload_len);
    ip[0] = 0x45; ip[8] = 64; ip[9] = 17;
    put16(ip + 2, ip_len); put16(ip + 4, 1); put16(ip + 6, 0x4000U);
    put32(ip + 12, source_ip); put32(ip + 16, target_ip);
    put16(ip + 10, 0);
    uint16_t ip_sum = checksum(ip, 20);
    put16(ip + 10, bad_ip_checksum ? (uint16_t)(ip_sum ^ 0x0100U) : ip_sum);
    uint8_t* udp = ip + 20;
    put16(udp, source_port); put16(udp + 2, target_port);
    put16(udp + 4, (uint16_t)(8U + payload_len));
    put16(udp + 6, 0); /* IPv4 permits an omitted UDP checksum */
    if (payload_len && payload) memcpy(udp + 8, payload, payload_len);
    *out_len = 14U + ip_len;
}
static void make_arp_reply(uint8_t* out, uint32_t* out_len, uint32_t sender_ip)
{
    memset(out, 0, 1600);
    memcpy(out, LOCAL_MAC, 6); memcpy(out + 6, DNS_MAC, 6); put16(out + 12, 0x0806U);
    uint8_t* a = out + 14;
    put16(a, 1); put16(a + 2, 0x0800); a[4] = 6; a[5] = 4; put16(a + 6, 2);
    memcpy(a + 8, DNS_MAC, 6); put32(a + 14, sender_ip);
    memcpy(a + 18, LOCAL_MAC, 6); put32(a + 24, LOCAL_IP);
    *out_len = 42;
}
static void maybe_reply_to_dns(const uint8_t* frame, uint32_t length)
{
    if (length < 14U + 20U + 8U || be16(frame + 12) != 0x0800U) return;
    const uint8_t* ip = frame + 14;
    if ((ip[0] >> 4) != 4 || ip[9] != 17 || be16(ip + 2) > length - 14U) return;
    uint8_t ihl = (uint8_t)((ip[0] & 0x0FU) * 4U);
    if (ihl < 20 || length < 14U + ihl + 8U) return;
    const uint8_t* udp = ip + ihl;
    if (be16(udp + 2) != 53U) return;
    uint16_t udp_len = be16(udp + 4);
    if (udp_len < 8U + 12U || 14U + ihl + udp_len > length) return;
    const uint8_t* query = udp + 8;
    uint32_t query_len = udp_len - 8U;
    uint32_t qend = 12U, steps = 0;
    while (qend < query_len && steps++ <= query_len)
    {
        uint8_t label = query[qend++];
        if (label == 0) break;
        if (label > 63U || qend + label > query_len) return;
        qend += label;
    }
    if (qend + 4U > query_len) return;
    qend += 4U;

    uint8_t response[512]; memset(response, 0, sizeof(response));
    put16(response, be16(query)); put16(response + 2, 0x8180U);
    put16(response + 4, 1); put16(response + 6, 1);
    memcpy(response + 12, query + 12, qend - 12U);
    uint32_t off = 12U + qend - 12U;
    response[off++] = 0xC0; response[off++] = 0x0C;
    put16(response + off, 1); off += 2; put16(response + off, 1); off += 2;
    response[off++] = 0; response[off++] = 0; response[off++] = 0; response[off++] = 60;
    put16(response + off, 4); off += 2; put32(response + off, ANSWER_IP); off += 4;

    uint8_t outgoing[1600]; uint32_t outgoing_len = 0;
    make_ipv4_udp_frame(outgoing, &outgoing_len, DNS_IP, 53, LOCAL_IP,
                        be16(udp), response, off, 0);
    (void)enqueue_frame(outgoing, outgoing_len);
}

void uart_putc(char) {}
void uart_puts(const char*) {}
void virtio_net_init() { fake_present = fake_ready = 1; }
int virtio_net_present() { return fake_present; }
int virtio_net_ready() { return fake_ready; }
void virtio_net_get_mac(uint8_t mac[6]) { memcpy(mac, LOCAL_MAC, 6); }
int virtio_net_send(const void* frame, uint32_t length)
{
    if (!frame || length > sizeof(last_tx)) return -22;
    memcpy(last_tx, frame, length); last_tx_length = length;
    maybe_reply_to_dns((const uint8_t*)frame, length);
    return 0;
}
int virtio_net_receive(void* frame, uint32_t capacity)
{
    if (!frame || capacity == 0) return -22;
    if (rx_count == 0) return 0;
    unsigned slot = rx_head;
    uint32_t length = rx_lengths[slot];
    if (length > capacity) length = capacity;
    memcpy(frame, rx_queue[slot], length);
    rx_head = (rx_head + 1U) % 16U;
    --rx_count;
    return (int)length;
}
uint64_t virtio_net_base() { return 0; }
int virtio_net_irq() { return -1; }
void virtio_net_status() {}
uint64_t timer_millis() { return fake_ms++; }

int main()
{
    uint32_t ip = 0;
    CHECK(net_parse_ipv4("10.0.2.15", &ip) && ip == LOCAL_IP, "valid IPv4 parse");
    CHECK(net_parse_ipv4("255.255.255.255", &ip), "broadcast IPv4 parse");
    CHECK(!net_parse_ipv4("10.0.2.999", &ip), "reject octet >255");
    CHECK(!net_parse_ipv4("10.0.2", &ip), "reject short IPv4");
    CHECK(!net_parse_ipv4("10..2.15", &ip), "reject empty octet");
    CHECK(!net_parse_ipv4("10.0.2.15x", &ip), "reject trailing garbage");

    net_init();
    CHECK(net_ready(), "network init ready");
    uint8_t frame[1600]; uint32_t frame_len = 0;
    make_arp_reply(frame, &frame_len, DNS_IP);
    CHECK(enqueue_frame(frame, frame_len), "enqueue ARP reply");
    CHECK(net_poll_once() == 1, "consume ARP reply and cache neighbor");

    int socket = net_udp_socket(77);
    int other = net_udp_socket(88);
    CHECK(socket > 0 && other > 0 && socket != other, "allocate owner-scoped UDP sockets");
    CHECK(net_udp_bind(77, socket, 5300) == 0, "bind UDP socket");
    CHECK(net_udp_bind(88, other, 5300) == -98, "reject duplicate local port");
    CHECK(net_udp_bind(88, socket, 5301) == -9, "reject cross-owner socket access");

    const uint8_t greeting[] = {'h','e','l','l','o'};
    make_ipv4_udp_frame(frame, &frame_len, DNS_IP, 12345, LOCAL_IP, 5300, greeting, sizeof(greeting), 0);
    CHECK(enqueue_frame(frame, frame_len), "enqueue UDP datagram");
    CHECK(net_poll_once() == 1, "parse UDP packet");
    char received[16] = {0}; uint32_t source_ip = 0; uint16_t source_port = 0;
    int got = net_udp_recvfrom(77, socket, &source_ip, &source_port, received, sizeof(received), 0);
    CHECK(got == 5 && memcmp(received, "hello", 5) == 0, "receive payload");
    CHECK(source_ip == DNS_IP && source_port == 12345, "receive source address");

    /* A present, valid UDP checksum is accepted; a corrupt one is discarded. */
    make_ipv4_udp_frame(frame, &frame_len, DNS_IP, 12345, LOCAL_IP, 5300, greeting, sizeof(greeting), 0);
    uint8_t* valid_udp = frame + 14U + 20U;
    uint16_t valid_udp_sum = udp_checksum_value(DNS_IP, LOCAL_IP, valid_udp, 8U + sizeof(greeting));
    if (valid_udp_sum == 0) valid_udp_sum = 0xFFFFU;
    put16(valid_udp + 6, valid_udp_sum);
    CHECK(enqueue_frame(frame, frame_len), "enqueue UDP datagram with valid checksum");
    CHECK(net_poll_once() == 1, "consume UDP datagram with valid checksum");
    memset(received, 0, sizeof(received));
    CHECK(net_udp_recvfrom(77, socket, &source_ip, &source_port, received, sizeof(received), 0) == 5 &&
          memcmp(received, "hello", 5) == 0, "valid UDP checksum accepted");

    make_ipv4_udp_frame(frame, &frame_len, DNS_IP, 12345, LOCAL_IP, 5300, greeting, sizeof(greeting), 0);
    put16(frame + 14U + 20U + 6U, 0x1234U);
    CHECK(enqueue_frame(frame, frame_len), "enqueue bad UDP checksum");
    CHECK(net_poll_once() == 1, "consume packet with bad UDP checksum");
    CHECK(net_udp_recvfrom(77, socket, &source_ip, &source_port, received, sizeof(received), 0) == -110,
          "bad UDP checksum not delivered");
    CHECK(net_udp_recvfrom(77, socket, &source_ip, &source_port, received, sizeof(received), 0) == -110,
          "empty socket returns timeout");

    /* Bad IPv4 checksums must not inject data into a UDP socket. */
    make_ipv4_udp_frame(frame, &frame_len, DNS_IP, 12345, LOCAL_IP, 5300, greeting, sizeof(greeting), 1);
    CHECK(enqueue_frame(frame, frame_len), "enqueue malformed IPv4 packet");
    CHECK(net_poll_once() == -5, "reject corrupt IPv4 checksum");
    CHECK(net_udp_recvfrom(77, socket, &source_ip, &source_port, received, sizeof(received), 0) == -110,
          "corrupt packet not delivered");

    const char outgoing[] = "udp!";
    CHECK(net_udp_sendto(77, socket, DNS_IP, 9000, outgoing, 4) == 4, "send UDP datagram");
    CHECK(last_tx_length >= 14U + 20U + 8U + 4U, "capture outgoing UDP frame");
    CHECK(be16(last_tx + 12) == 0x0800U, "outgoing ethernet IPv4 type");
    const uint8_t* out_ip = last_tx + 14;
    CHECK(out_ip[9] == 17 && checksum(out_ip, 20) == 0, "outgoing IPv4 header/checksum");
    const uint8_t* out_udp = out_ip + 20;
    CHECK(be16(out_udp + 2) == 9000 && be16(out_udp + 4) == 12, "outgoing UDP header");
    CHECK(memcmp(out_udp + 8, outgoing, 4) == 0, "outgoing UDP payload");
    CHECK(be16(out_udp + 6) != 0, "outgoing UDP checksum generated");
    CHECK(udp_checksum_valid(LOCAL_IP, DNS_IP, out_udp, be16(out_udp + 4)),
          "outgoing UDP pseudo-header checksum validates");

    uint32_t resolved_ip = 0;
    CHECK(net_dns_resolve("example.com", &resolved_ip, 1000) == 0, "DNS A lookup end-to-end with fake peer");
    CHECK(resolved_ip == ANSWER_IP, "DNS resolver returns A record");
    CHECK(net_dns_resolve("bad/name", &resolved_ip, 1000) == -22, "reject invalid DNS hostname");
    CHECK(net_dns_set_server(0) == -22, "reject invalid DNS server");
    CHECK(net_dns_server() == DNS_IP, "default DNS server retained");

    CHECK(net_udp_close(88, socket) == -9, "wrong owner cannot close socket");
    CHECK(net_udp_close(77, socket) == 0, "close UDP socket");
    CHECK(net_udp_sendto(77, socket, DNS_IP, 9000, outgoing, 4) == -9, "closed socket rejected");
    net_udp_close_all_for(88);
    CHECK(net_udp_bind(88, other, 0) == -9, "close-all releases process sockets");

    puts("NET HOST TEST: PASS (IPv4, ARP, UDP sockets/checksum, DNS A lookup)");
    return 0;
}
