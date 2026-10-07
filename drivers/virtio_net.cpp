#include "virtio_net.h"
#include "virtio_mmio.h"
#include "uart.h"
#include "timer.h"

#define V_MAGIC                 0x000
#define V_VERSION               0x004
#define V_DEVICE_ID             0x008
#define V_DEVICE_FEATURES       0x010
#define V_DEVICE_FEATURES_SEL   0x014
#define V_DRIVER_FEATURES       0x020
#define V_DRIVER_FEATURES_SEL   0x024
#define V_QUEUE_SEL             0x030
#define V_QUEUE_NUM_MAX         0x034
#define V_QUEUE_NUM              0x038
#define V_QUEUE_READY            0x044
#define V_QUEUE_NOTIFY           0x050
#define V_STATUS                 0x070
#define V_QUEUE_DESC_LOW         0x080
#define V_QUEUE_DESC_HIGH        0x084
#define V_QUEUE_DRIVER_LOW       0x090
#define V_QUEUE_DRIVER_HIGH      0x094
#define V_QUEUE_DEVICE_LOW       0x0A0
#define V_QUEUE_DEVICE_HIGH      0x0A4
#define V_CONFIG                 0x100

#define VIRTIO_NET_DEVICE_ID     1U
#define VIRTIO_F_VERSION_1       32U
#define VIRTIO_NET_F_MAC         5U
#define STATUS_ACKNOWLEDGE       0x01U
#define STATUS_DRIVER            0x02U
#define STATUS_DRIVER_OK         0x04U
#define STATUS_FEATURES_OK       0x08U
#define STATUS_FAILED             0x80U
#define DESC_NEXT                1U
#define DESC_WRITE               2U
#define QUEUE_MAX                8U
#define NET_HDR_SIZE             12U
#define RX_BUFFER_SIZE           2048U
#define TX_BUFFER_SIZE           2048U
#define REQUEST_TIMEOUT_MS       2000ULL

struct VirtqDesc { uint64_t addr; uint32_t len; uint16_t flags; uint16_t next; };
struct VirtqAvail { uint16_t flags; uint16_t idx; uint16_t ring[QUEUE_MAX]; uint16_t used_event; };
struct VirtqUsedElem { uint32_t id; uint32_t len; };
struct VirtqUsed { uint16_t flags; uint16_t idx; VirtqUsedElem ring[QUEUE_MAX]; uint16_t avail_event; };

static volatile uint32_t* mmio = 0;
static uint64_t base_addr = 0;
static int present = 0;
static int ready = 0;
static int queue_size = 0;
static int device_irq = -1;
static uint8_t mac_address[6];
static uint16_t tx_last_used = 0;
static uint16_t rx_last_used = 0;

static VirtqDesc rx_desc[QUEUE_MAX] __attribute__((aligned(16), section(".bss.virtio_net")));
static VirtqAvail rx_avail __attribute__((aligned(16), section(".bss.virtio_net")));
static VirtqUsed rx_used __attribute__((aligned(16), section(".bss.virtio_net")));
static uint8_t rx_buffers[QUEUE_MAX][RX_BUFFER_SIZE] __attribute__((aligned(16), section(".bss.virtio_net")));

static VirtqDesc tx_desc[1] __attribute__((aligned(16), section(".bss.virtio_net")));
static VirtqAvail tx_avail __attribute__((aligned(16), section(".bss.virtio_net")));
static VirtqUsed tx_used __attribute__((aligned(16), section(".bss.virtio_net")));
static uint8_t tx_buffer[TX_BUFFER_SIZE] __attribute__((aligned(16), section(".bss.virtio_net")));

static void barrier()
{
    asm volatile("dmb osh\n dsb sy\n" ::: "memory");
}

static void mmio_write(uint32_t off, uint32_t value)
{
    mmio[off / 4U] = value;
}

static uint32_t mmio_read(uint32_t off)
{
    return mmio[off / 4U];
}

static void zero_queue(VirtqDesc* d, VirtqAvail* a, VirtqUsed* u, int n)
{
    for (int i = 0; i < n; ++i)
    {
        d[i].addr = 0; d[i].len = 0; d[i].flags = 0; d[i].next = 0;
        a->ring[i] = 0;
        u->ring[i].id = 0; u->ring[i].len = 0;
    }
    a->flags = 0; a->idx = 0; a->used_event = 0;
    u->flags = 0; u->idx = 0; u->avail_event = 0;
}

static int setup_queue(uint16_t index, VirtqDesc* d, VirtqAvail* a, VirtqUsed* u, int max_local)
{
    mmio_write(V_QUEUE_SEL, index);
    uint32_t max = mmio_read(V_QUEUE_NUM_MAX);
    if (max < 1) return 0;
    int n = (int)max;
    if (n > max_local) n = max_local;
    if (n < 1) return 0;
    if (mmio_read(V_QUEUE_READY) != 0) return 0;
    zero_queue(d, a, u, n);
    mmio_write(V_QUEUE_NUM, (uint32_t)n);
    uint64_t da = (uint64_t)d, aa = (uint64_t)a, ua = (uint64_t)u;
    mmio_write(V_QUEUE_DESC_LOW, (uint32_t)da);
    mmio_write(V_QUEUE_DESC_HIGH, (uint32_t)(da >> 32));
    mmio_write(V_QUEUE_DRIVER_LOW, (uint32_t)aa);
    mmio_write(V_QUEUE_DRIVER_HIGH, (uint32_t)(aa >> 32));
    mmio_write(V_QUEUE_DEVICE_LOW, (uint32_t)ua);
    mmio_write(V_QUEUE_DEVICE_HIGH, (uint32_t)(ua >> 32));
    barrier();
    mmio_write(V_QUEUE_READY, 1);
    return mmio_read(V_QUEUE_READY) == 1;
}

static int wait_used(VirtqUsed* used, uint16_t old_idx)
{
    uint64_t start = timer_millis();
    while (used->idx == old_idx)
    {
        barrier();
        if (timer_millis() - start >= REQUEST_TIMEOUT_MS) return -110;
    }
    return 0;
}

void virtio_net_init()
{
    present = ready = 0;
    queue_size = 0; device_irq = -1; base_addr = 0;
    for (int i = 0; i < 6; ++i) mac_address[i] = 0;

    uint64_t base = 0, size = 0; int irq = -1;
    if (!virtio_mmio_find_device(VIRTIO_NET_DEVICE_ID, &base, &size, &irq))
    {
        uart_puts("VirtIO Net: device not found.\r\n");
        return;
    }

    base_addr = base; device_irq = irq; mmio = (volatile uint32_t*)base;
    if (mmio_read(V_MAGIC) != 0x74726976U || mmio_read(V_VERSION) != 2U || mmio_read(V_DEVICE_ID) != VIRTIO_NET_DEVICE_ID)
    {
        uart_puts("VirtIO Net: unsupported transport.\r\n");
        mmio = 0; return;
    }
    present = 1;

    mmio_write(V_STATUS, 0);
    mmio_write(V_STATUS, STATUS_ACKNOWLEDGE);
    mmio_write(V_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER);

    mmio_write(V_DEVICE_FEATURES_SEL, 0);
    uint32_t features_lo = mmio_read(V_DEVICE_FEATURES);
    mmio_write(V_DEVICE_FEATURES_SEL, 1);
    uint32_t features_hi = mmio_read(V_DEVICE_FEATURES);
    if ((features_hi & (1U << (VIRTIO_F_VERSION_1 - 32U))) == 0 || (features_lo & (1U << VIRTIO_NET_F_MAC)) == 0)
    {
        uart_puts("VirtIO Net: required features unavailable.\r\n");
        mmio_write(V_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER | STATUS_FAILED);
        return;
    }

    mmio_write(V_DRIVER_FEATURES_SEL, 0);
    mmio_write(V_DRIVER_FEATURES, (1U << VIRTIO_NET_F_MAC));
    mmio_write(V_DRIVER_FEATURES_SEL, 1);
    mmio_write(V_DRIVER_FEATURES, (1U << (VIRTIO_F_VERSION_1 - 32U)));
    mmio_write(V_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER | STATUS_FEATURES_OK);
    barrier();
    if ((mmio_read(V_STATUS) & STATUS_FEATURES_OK) == 0)
    {
        uart_puts("VirtIO Net: FEATURES_OK rejected.\r\n");
        return;
    }

    volatile uint8_t* config8 = (volatile uint8_t*)((uint64_t)mmio + V_CONFIG);
    for (int i = 0; i < 6; ++i)
        mac_address[i] = config8[i];

    mmio_write(V_QUEUE_SEL, 0);
    uint32_t rxmax = mmio_read(V_QUEUE_NUM_MAX);
    if (rxmax < 1) { uart_puts("VirtIO Net: RX queue unavailable.\r\n"); return; }
    queue_size = (int)rxmax; if (queue_size > (int)QUEUE_MAX) queue_size = (int)QUEUE_MAX;
    if (queue_size < 1) return;

    if (!setup_queue(0, rx_desc, &rx_avail, &rx_used, QUEUE_MAX) || !setup_queue(1, tx_desc, &tx_avail, &tx_used, 1))
    {
        uart_puts("VirtIO Net: queue setup failed.\r\n");
        return;
    }

    tx_last_used = 0; rx_last_used = 0;
    for (int i = 0; i < queue_size; ++i)
    {
        rx_desc[i].addr = (uint64_t)&rx_buffers[i][0];
        rx_desc[i].len = RX_BUFFER_SIZE;
        rx_desc[i].flags = DESC_WRITE;
        rx_desc[i].next = 0;
        rx_avail.ring[i] = (uint16_t)i;
    }
    barrier();
    rx_avail.idx = (uint16_t)queue_size;
    barrier();
    mmio_write(V_QUEUE_SEL, 0);
    mmio_write(V_QUEUE_NOTIFY, 0);

    mmio_write(V_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER | STATUS_FEATURES_OK | STATUS_DRIVER_OK);
    barrier();
    if ((mmio_read(V_STATUS) & STATUS_DRIVER_OK) == 0)
    {
        uart_puts("VirtIO Net: DRIVER_OK rejected.\r\n");
        mmio_write(V_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER | STATUS_FEATURES_OK | STATUS_FAILED);
        return;
    }
    ready = 1;
    uart_puts("VirtIO Net: ready.\r\n");
}

int virtio_net_present() { return present; }
int virtio_net_ready() { return ready; }
void virtio_net_get_mac(uint8_t mac[6]) { if (!mac) return; for (int i=0;i<6;i++) mac[i]=mac_address[i]; }
uint64_t virtio_net_base() { return base_addr; }
int virtio_net_irq() { return device_irq; }

int virtio_net_send(const void* frame, uint32_t length)
{
    if (!ready || !frame || length == 0 || length > (TX_BUFFER_SIZE - NET_HDR_SIZE)) return -22;
    for (uint32_t i = 0; i < NET_HDR_SIZE; ++i) tx_buffer[i] = 0;
    const uint8_t* src = (const uint8_t*)frame;
    for (uint32_t i = 0; i < length; ++i) tx_buffer[NET_HDR_SIZE + i] = src[i];
    tx_desc[0].addr = (uint64_t)tx_buffer;
    tx_desc[0].len = NET_HDR_SIZE + length;
    tx_desc[0].flags = 0;
    tx_desc[0].next = 0;
    uint16_t old = tx_avail.idx;
    barrier();
    tx_avail.ring[old % 1] = 0;
    tx_avail.idx = (uint16_t)(old + 1U);
    barrier();
    mmio_write(V_QUEUE_SEL, 1);
    mmio_write(V_QUEUE_NOTIFY, 1);
    if (wait_used(&tx_used, tx_last_used) < 0) return -110;
    if (tx_used.ring[tx_last_used % 1].id != 0) return -5;
    tx_last_used = (uint16_t)(tx_last_used + 1U);
    return 0;
}

int virtio_net_receive(void* frame, uint32_t capacity)
{
    if (!ready || !frame || capacity == 0) return -22;
    if (rx_used.idx == rx_last_used) return 0;
    uint16_t slot = (uint16_t)(rx_last_used % (uint16_t)queue_size);
    volatile VirtqUsedElem* elem = &rx_used.ring[slot];
    if (elem->id >= (uint32_t)queue_size)
    {
        rx_last_used = (uint16_t)(rx_last_used + 1U);
        return -5;
    }
    if (elem->len < NET_HDR_SIZE)
    {
        rx_avail.ring[rx_avail.idx % (uint16_t)queue_size] = (uint16_t)elem->id;
        barrier();
        rx_avail.idx = (uint16_t)(rx_avail.idx + 1U);
        barrier();
        mmio_write(V_QUEUE_SEL, 0);
        mmio_write(V_QUEUE_NOTIFY, 0);
        rx_last_used = (uint16_t)(rx_last_used + 1U);
        return -5;
    }
    uint32_t frame_len = elem->len - NET_HDR_SIZE;
    if (frame_len > capacity) frame_len = capacity;
    const uint8_t* src = &rx_buffers[elem->id][NET_HDR_SIZE];
    uint8_t* dst = (uint8_t*)frame;
    for (uint32_t i = 0; i < frame_len; ++i) dst[i] = src[i];

    rx_avail.ring[rx_avail.idx % (uint16_t)queue_size] = (uint16_t)elem->id;
    barrier();
    rx_avail.idx = (uint16_t)(rx_avail.idx + 1U);
    barrier();
    mmio_write(V_QUEUE_SEL, 0);
    mmio_write(V_QUEUE_NOTIFY, 0);
    rx_last_used = (uint16_t)(rx_last_used + 1U);
    return (int)frame_len;
}

static void print_hex8(uint8_t v) { static const char h[]="0123456789ABCDEF"; uart_putc(h[v>>4]); uart_putc(h[v&15]); }
void virtio_net_status()
{
    uart_puts("VirtIO Net status\r\n-----------------\r\nPresent : ");
    uart_puts(present ? "yes\r\n" : "no\r\n");
    uart_puts("Ready   : "); uart_puts(ready ? "yes\r\n" : "no\r\n");
    uart_puts("MAC     : ");
    for (int i=0;i<6;i++){ print_hex8(mac_address[i]); if(i!=5) uart_putc(':'); }
    uart_puts("\r\nIRQ     : ");
    if (device_irq < 0) uart_puts("unknown\r\n");
    else { char b[16]; int p=0; int v=device_irq; if(v==0)b[p++]='0'; while(v){b[p++]=(char)('0'+v%10);v/=10;} while(p)uart_putc(b[--p]); uart_puts("\r\n"); }
}
