#include "nvme.h"
#include "pci.h"
#include "uart.h"

#define Q_DEPTH 16

static uint8_t admin_sq[4096] __attribute__((aligned(4096)));
static uint8_t admin_cq[4096] __attribute__((aligned(4096)));
static uint8_t io_sq[4096] __attribute__((aligned(4096)));
static uint8_t io_cq[4096] __attribute__((aligned(4096)));
static uint8_t data_buf[4096] __attribute__((aligned(4096)));

static uint64_t reg_base = 0;
static uint32_t stride = 4;
static int ready = 0;
static uint64_t sectors = 0;
static uint16_t a_tail = 0, a_head = 0, i_tail = 0, i_head = 0;
static uint8_t a_phase = 1, i_phase = 1;
static uint16_t cid_next = 1;
static uint16_t pci_dev_id = 0;

static volatile uint32_t* reg32(uint64_t off) { return (volatile uint32_t*)(reg_base + off); }
static uint64_t read64(uint64_t off) { return (uint64_t)*reg32(off) | ((uint64_t)*reg32(off + 4) << 32); }
static void write64(uint64_t off, uint64_t v) { *reg32(off) = (uint32_t)v; *reg32(off + 4) = (uint32_t)(v >> 32); }
static void dsb() { asm volatile("dsb sy" ::: "memory"); }

static int wait_ready(int want)
{
    for (int i = 0; i < 2000000; ++i)
    {
        if (((*reg32(0x1C)) & 1U) == (uint32_t)want) return 0;
        asm volatile("yield");
    }
    return -1;
}

static void wr32(uint8_t* p, int dw, uint32_t v) { ((volatile uint32_t*)p)[dw] = v; }

/* queue: 0 admin, 1 io. returns completion status (0 = ok) or -1 on timeout */
static int submit(int queue, uint32_t opcode, uint32_t nsid, uint64_t prp1,
                  uint32_t cdw10, uint32_t cdw11, uint32_t cdw12)
{
    uint8_t* sq = queue ? io_sq : admin_sq;
    uint8_t* cq = queue ? io_cq : admin_cq;
    uint16_t* tail = queue ? &i_tail : &a_tail;
    uint16_t* head = queue ? &i_head : &a_head;
    uint8_t* phase = queue ? &i_phase : &a_phase;
    uint16_t cid = cid_next++;
    uint8_t* e = sq + (uint64_t)(*tail) * 64U;
    for (int i = 0; i < 16; ++i) wr32(e, i, 0);
    wr32(e, 0, opcode | ((uint32_t)cid << 16));
    wr32(e, 1, nsid);
    wr32(e, 6, (uint32_t)prp1); wr32(e, 7, (uint32_t)(prp1 >> 32));
    wr32(e, 10, cdw10); wr32(e, 11, cdw11); wr32(e, 12, cdw12);
    *tail = (uint16_t)((*tail + 1) % Q_DEPTH);
    dsb();
    *reg32(0x1000 + (uint64_t)(2 * queue) * stride) = *tail;
    dsb();
    for (int spin = 0; spin < 40000000; ++spin)
    {
        volatile uint32_t* c = (volatile uint32_t*)(cq + (uint64_t)(*head) * 16U);
        uint32_t dw3 = c[3];
        if (((dw3 >> 16) & 1U) == *phase)
        {
            uint32_t status = dw3 >> 17;
            *head = (uint16_t)((*head + 1) % Q_DEPTH);
            if (*head == 0) *phase ^= 1U;
            dsb();
            *reg32(0x1000 + (uint64_t)(2 * queue + 1) * stride) = *head;
            return (int)status;
        }
        asm volatile("yield");
    }
    return -1;
}

void nvme_init()
{
    ready = 0; sectors = 0;
    const PciDevice* d = pci_find_class(1, 8, 0xFF, 0);
    if (!d || d->bar_size[0] == 0) return;
    reg_base = d->bar[0];
    pci_dev_id = d->device;
    uint64_t cap = read64(0x00);
    stride = 4U << ((cap >> 32) & 0xFU);
    *reg32(0x14) = 0;                           /* CC.EN = 0 */
    dsb();
    if (wait_ready(0) != 0) { uart_puts("NVMe: reset timeout\r\n"); return; }
    for (int i = 0; i < 4096; ++i) { admin_sq[i] = 0; admin_cq[i] = 0; io_sq[i] = 0; io_cq[i] = 0; }
    a_tail = a_head = i_tail = i_head = 0; a_phase = i_phase = 1;
    *reg32(0x24) = ((uint32_t)(Q_DEPTH - 1) << 16) | (uint32_t)(Q_DEPTH - 1);
    write64(0x28, (uint64_t)admin_sq);
    write64(0x30, (uint64_t)admin_cq);
    dsb();
    *reg32(0x14) = (6U << 16) | (4U << 20) | 1U;      /* IOSQES=64B, IOCQES=16B, MPS=4K, EN */
    dsb();
    if (wait_ready(1) != 0) { uart_puts("NVMe: enable timeout\r\n"); return; }
    uint32_t qs = (uint32_t)(Q_DEPTH - 1) << 16;
    if (submit(0, 0x05, 0, (uint64_t)io_cq, qs | 1U, 1U, 0) != 0) { uart_puts("NVMe: create IO CQ failed\r\n"); return; }
    int src = submit(0, 0x01, 0, (uint64_t)io_sq, qs | 1U, (1U << 16) | 1U, 0);
    if (src != 0) { uart_puts("NVMe: create IO SQ failed, status "); uart_putc((char)('0' + ((src / 100) % 10))); uart_putc((char)('0' + ((src / 10) % 10))); uart_putc((char)('0' + (src % 10))); uart_puts("\r\n"); return; }
    for (int i = 0; i < 4096; ++i) data_buf[i] = 0;
    if (submit(0, 0x06, 1, (uint64_t)data_buf, 0, 0, 0) != 0) { uart_puts("NVMe: identify namespace failed\r\n"); return; }
    volatile uint32_t* id = (volatile uint32_t*)data_buf;
    uint64_t nsze = (uint64_t)id[0] | ((uint64_t)id[1] << 32);
    uint32_t flbas = data_buf[26] & 0xFU;
    uint32_t lbaf = *(volatile uint32_t*)(data_buf + 128 + flbas * 4);
    uint32_t lbads = (lbaf >> 16) & 0xFFU;
    if (lbads != 9) { uart_puts("NVMe: unsupported LBA size (only 512 B)\r\n"); return; }
    sectors = nsze;
    ready = 1;
    uart_puts("NVMe: controller ready, namespace 1 present.\r\n");
}

int nvme_ready() { return ready; }
uint64_t nvme_capacity() { return sectors; }

int nvme_read(uint64_t sector, void* buffer)
{
    if (!ready || sector >= sectors) return -1;
    if (submit(1, 0x02, 1, (uint64_t)data_buf, (uint32_t)sector, (uint32_t)(sector >> 32), 0) != 0) return -1;
    for (int i = 0; i < 512; ++i) ((uint8_t*)buffer)[i] = ((volatile uint8_t*)data_buf)[i];
    return 0;
}

int nvme_write(uint64_t sector, const void* buffer)
{
    if (!ready || sector >= sectors) return -1;
    for (int i = 0; i < 512; ++i) ((volatile uint8_t*)data_buf)[i] = ((const uint8_t*)buffer)[i];
    return submit(1, 0x01, 1, (uint64_t)data_buf, (uint32_t)sector, (uint32_t)(sector >> 32), 0) == 0 ? 0 : -1;
}

int nvme_flush() { return ready ? (submit(1, 0x00, 1, 0, 0, 0, 0) == 0 ? 0 : -1) : -1; }

static void put_dec(uint64_t v)
{
    char t[24]; int n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) uart_putc(t[--n]);
}

void nvme_status()
{
    if (!ready) { uart_puts("NVMe: no controller/namespace\r\n"); return; }
    uart_puts("NVMe: namespace 1, "); put_dec(sectors); uart_puts(" sectors of 512 B ("); put_dec(sectors / 2048); uart_puts(" MiB), BAR0 0x");
    for (int i = 7; i >= 0; --i) { int dgt = (int)((reg_base >> (i * 4)) & 0xF); uart_putc((char)(dgt < 10 ? '0' + dgt : 'a' + dgt - 10)); }
    uart_puts("\r\n");
}

int nvme_selftest(int destructive)
{
    if (!ready) return -1;
    static uint8_t a[512], b[512];
    if (nvme_read(0, a) != 0) return -2;
    if (nvme_read(0, b) != 0) return -2;
    for (int i = 0; i < 512; ++i) if (a[i] != b[i]) return -3;
    if (!destructive) return 0;
    uint64_t last = sectors - 1;
    uint8_t save[512];
    if (nvme_read(last, save) != 0) return -4;
    for (int i = 0; i < 512; ++i) a[i] = (uint8_t)(i * 7 + 3);
    if (nvme_write(last, a) != 0) return -5;
    if (nvme_flush() != 0) return -6;
    if (nvme_read(last, b) != 0) return -7;
    int bad = 0;
    for (int i = 0; i < 512; ++i) if (a[i] != b[i]) bad = 1;
    (void)nvme_write(last, save);
    return bad ? -8 : 0;
}
