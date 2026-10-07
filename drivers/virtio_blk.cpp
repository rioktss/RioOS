#include "virtio_blk.h"
#include "uart.h"
#include "timer.h"
#include "memory.h"
#include "virtio_mmio.h"

/* QEMU virt: virtio-mmio transport region begins at 0x0a000000.
 * The driver normally discovers the exact instance from the FDT at RAM+0,
 * with a conservative fallback to the first transport for old/simple setups.
 */
#define FDT_ADDR                0x40000000ULL
#define FDT_MAGIC               0xD00DFEEDU
#define FDT_BEGIN_NODE          1U
#define FDT_END_NODE            2U
#define FDT_PROP                3U
#define FDT_NOP                 4U
#define FDT_END                 9U

#define DEFAULT_MMIO_BASE       0x0A000000ULL
#define MAX_FDT_SCAN            (1024U * 1024U)

/* VirtIO MMIO register offsets, modern (version 2) transport. */
#define V_MAGIC                 0x000
#define V_VERSION               0x004
#define V_DEVICE_ID             0x008
#define V_VENDOR_ID             0x00C
#define V_DEVICE_FEATURES       0x010
#define V_DEVICE_FEATURES_SEL   0x014
#define V_DRIVER_FEATURES       0x020
#define V_DRIVER_FEATURES_SEL   0x024
#define V_QUEUE_SEL             0x030
#define V_QUEUE_NUM_MAX         0x034
#define V_QUEUE_NUM              0x038
#define V_QUEUE_READY            0x044
#define V_QUEUE_NOTIFY           0x050
#define V_INTERRUPT_STATUS       0x060
#define V_INTERRUPT_ACK          0x064
#define V_STATUS                 0x070
#define V_QUEUE_DESC_LOW         0x080
#define V_QUEUE_DESC_HIGH        0x084
#define V_QUEUE_DRIVER_LOW       0x090
#define V_QUEUE_DRIVER_HIGH      0x094
#define V_QUEUE_DEVICE_LOW       0x0A0
#define V_QUEUE_DEVICE_HIGH      0x0A4
#define V_CONFIG_GENERATION      0x0FC
#define V_CONFIG                0x100

#define VIRTIO_MAGIC             0x74726976U
#define VIRTIO_VERSION           2U
#define VIRTIO_DEVICE_BLOCK      2U

#define STATUS_ACKNOWLEDGE       0x01U
#define STATUS_DRIVER            0x02U
#define STATUS_DRIVER_OK         0x04U
#define STATUS_FEATURES_OK       0x08U
#define STATUS_FAILED            0x80U
#define STATUS_NEEDS_RESET       0x40U

#define VIRTIO_F_VERSION_1       32U
#define VIRTIO_BLK_F_FLUSH        9U
#define VIRTIO_BLK_F_RO           5U

#define DESC_NEXT                 1U
#define DESC_WRITE                2U

#define REQ_IN                    0U
#define REQ_OUT                   1U
#define REQ_FLUSH                 4U

#define STATUS_OK                 0U

#define QUEUE_MAX_LOCAL            8U
#define SECTOR_SIZE               512U
#define REQUEST_TIMEOUT_MS        2000ULL

struct VirtqDesc
{
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};

struct VirtqAvail
{
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[QUEUE_MAX_LOCAL];
    uint16_t used_event;
};

struct VirtqUsedElem
{
    uint32_t id;
    uint32_t len;
};

struct VirtqUsed
{
    uint16_t flags;
    uint16_t idx;
    VirtqUsedElem ring[QUEUE_MAX_LOCAL];
    uint16_t avail_event;
};

struct BlkHeader
{
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
};

static volatile uint32_t* mmio = 0;
static uint64_t device_base = 0;
static uint64_t capacity_sectors = 0;
static uint32_t logical_block_size = SECTOR_SIZE;
static uint32_t negotiated_features_low = 0;
static uint32_t negotiated_features_high = 0;
static int present = 0;
static int ready = 0;
static int read_only = 0;
static int queue_size = 0;
static int device_irq = -1;

static VirtqDesc descriptors[QUEUE_MAX_LOCAL]
    __attribute__((aligned(16), section(".bss.virtio_queue")));
static VirtqAvail available
    __attribute__((aligned(16), section(".bss.virtio_queue")));
static VirtqUsed used
    __attribute__((aligned(16), section(".bss.virtio_queue")));
static BlkHeader request_header
    __attribute__((aligned(16), section(".bss.virtio_req")));
static uint8_t request_data[SECTOR_SIZE]
    __attribute__((aligned(16), section(".bss.virtio_req")));
static uint8_t request_status
    __attribute__((aligned(1), section(".bss.virtio_req")));
static uint16_t last_used_idx = 0;

static int fdt_find_virtio(uint64_t* base, uint64_t* size, int* irq)
{
    return virtio_mmio_find_device(VIRTIO_DEVICE_BLOCK, base, size, irq);
}

static void mmio_write(uint32_t off, uint32_t value)
{
    mmio[off / 4U] = value;
}

static uint32_t mmio_read(uint32_t off)
{
    return mmio[off / 4U];
}

static uint64_t config_u64(uint32_t off)
{
    uint32_t low = mmio_read(off);
    uint32_t high = mmio_read(off + 4);
    return ((uint64_t)high << 32) | low;
}

static void memory_barrier()
{
    asm volatile("dmb osh\n dsb sy\n" ::: "memory");
}

static void clear_queue_memory()
{
    for (int i = 0; i < (int)QUEUE_MAX_LOCAL; ++i)
    {
        descriptors[i].addr = 0;
        descriptors[i].len = 0;
        descriptors[i].flags = 0;
        descriptors[i].next = 0;
        available.ring[i] = 0;
        used.ring[i].id = 0;
        used.ring[i].len = 0;
    }

    available.flags = 0;
    available.idx = 0;
    available.used_event = 0;
    used.flags = 0;
    used.idx = 0;
    used.avail_event = 0;
    request_status = 0xFF;
    last_used_idx = 0;
}

static int check_device()
{
    if (mmio == 0)
        return 0;

    if (mmio_read(V_MAGIC) != VIRTIO_MAGIC)
        return 0;
    if (mmio_read(V_VERSION) != VIRTIO_VERSION)
        return 0;
    if (mmio_read(V_DEVICE_ID) != VIRTIO_DEVICE_BLOCK)
        return 0;

    return 1;
}

static void fail_device()
{
    if (mmio != 0)
    {
        uint32_t status = mmio_read(V_STATUS);
        mmio_write(V_STATUS, status | STATUS_FAILED);
        memory_barrier();
    }
    ready = 0;
}

static int init_queue()
{
    mmio_write(V_QUEUE_SEL, 0);
    uint32_t max = mmio_read(V_QUEUE_NUM_MAX);
    if (max == 0)
        return 0;

    queue_size = (int)max;
    if (queue_size > (int)QUEUE_MAX_LOCAL)
        queue_size = (int)QUEUE_MAX_LOCAL;
    if (queue_size < 3)
        return 0;

    if (mmio_read(V_QUEUE_READY) != 0)
        return 0;

    clear_queue_memory();

    mmio_write(V_QUEUE_NUM, (uint32_t)queue_size);

    uint64_t desc = (uint64_t)descriptors;
    uint64_t avail = (uint64_t)&available;
    uint64_t used_area = (uint64_t)&used;

    mmio_write(V_QUEUE_DESC_LOW, (uint32_t)desc);
    mmio_write(V_QUEUE_DESC_HIGH, (uint32_t)(desc >> 32));
    mmio_write(V_QUEUE_DRIVER_LOW, (uint32_t)avail);
    mmio_write(V_QUEUE_DRIVER_HIGH, (uint32_t)(avail >> 32));
    mmio_write(V_QUEUE_DEVICE_LOW, (uint32_t)used_area);
    mmio_write(V_QUEUE_DEVICE_HIGH, (uint32_t)(used_area >> 32));

    memory_barrier();
    mmio_write(V_QUEUE_READY, 1);

    return mmio_read(V_QUEUE_READY) == 1;
}

static int wait_for_request(uint16_t expected_idx)
{
    uint64_t start = timer_millis();

    /* used.idx starts one below expected_idx and the transport bumps it
       to expected_idx once this request's descriptor chain has been
       consumed - QEMU's virtio-mmio can even do this synchronously
       inside the V_QUEUE_NOTIFY write above, before this loop is ever
       reached. The driver must therefore poll WHILE the value has NOT
       yet reached expected_idx; waiting on equality (as this line
       used to) reads "already done" as "still pending" and vice versa,
       so every request silently spun for the full REQUEST_TIMEOUT_MS
       and returned a timeout error even when the device had completed
       it instantly. */
    while (used.idx != expected_idx)
    {
        memory_barrier();

        uint32_t status = mmio_read(V_STATUS);
        if (status & STATUS_NEEDS_RESET)
            return -5;

        if (timer_millis() - start >= REQUEST_TIMEOUT_MS)
            return -110;
    }

    uint16_t slot = (uint16_t)((expected_idx - 1U) % (uint16_t)queue_size);
    volatile VirtqUsedElem* elem = &used.ring[slot];
    if (elem->id != 0)
        return -5;

    return 0;
}

static int submit_request(uint32_t type, uint64_t sector, const uint8_t* write_data, uint8_t* read_data)
{
    if (!ready)
        return -19;

    if (sector >= capacity_sectors && type != REQ_FLUSH)
        return -22;

    request_header.type = type;
    request_header.reserved = 0;
    request_header.sector = sector;
    request_status = 0xFF;

    if (type == REQ_IN)
    {
        for (int i = 0; i < (int)SECTOR_SIZE; ++i)
            request_data[i] = 0;
    }
    else if (type == REQ_OUT)
    {
        if (write_data == 0)
            return -22;
        for (int i = 0; i < (int)SECTOR_SIZE; ++i)
            request_data[i] = write_data[i];
    }

    descriptors[0].addr = (uint64_t)&request_header;
    descriptors[0].len = sizeof(BlkHeader);
    descriptors[0].flags = DESC_NEXT;
    descriptors[0].next = 1;

    if (type == REQ_FLUSH)
    {
        descriptors[1].addr = (uint64_t)&request_status;
        descriptors[1].len = 1;
        descriptors[1].flags = DESC_WRITE;
        descriptors[1].next = 0;
    }
    else
    {
        descriptors[1].addr = (uint64_t)request_data;
        descriptors[1].len = SECTOR_SIZE;
        descriptors[1].flags = DESC_NEXT | ((type == REQ_IN) ? DESC_WRITE : 0);
        descriptors[1].next = 2;

        descriptors[2].addr = (uint64_t)&request_status;
        descriptors[2].len = 1;
        descriptors[2].flags = DESC_WRITE;
        descriptors[2].next = 0;
    }

    uint16_t old_idx = available.idx;
    uint16_t new_idx = (uint16_t)(old_idx + 1U);
    available.ring[old_idx % (uint16_t)queue_size] = 0;

    memory_barrier();
    available.idx = new_idx;
    memory_barrier();
    mmio_write(V_QUEUE_NOTIFY, 0);

    int result = wait_for_request(new_idx);
    if (result < 0)
    {
        fail_device();
        return result;
    }

    if (request_status != STATUS_OK)
    {
        return -5;
    }

    if (type == REQ_IN && read_data != 0)
    {
        for (int i = 0; i < (int)SECTOR_SIZE; ++i)
            read_data[i] = request_data[i];
    }

    last_used_idx = new_idx;
    return 0;
}

void virtio_blk_init()
{
    present = 0;
    ready = 0;
    device_base = 0;
    capacity_sectors = 0;
    logical_block_size = SECTOR_SIZE;
    read_only = 0;
    device_irq = -1;
    negotiated_features_low = 0;
    negotiated_features_high = 0;

    uint64_t base = DEFAULT_MMIO_BASE;
    uint64_t size = 0x200;
    int irq = -1;

    if (fdt_find_virtio(&base, &size, &irq))
    {
        device_irq = irq;
    }

    if (size < 0x200)
        size = 0x200;

    device_base = base;
    mmio = (volatile uint32_t*)base;

    uart_puts("VirtIO Block: probing device...\r\n");

    if (!check_device())
    {
        uart_puts("VirtIO Block: no modern virtio-blk device found.\r\n");
        mmio = 0;
        return;
    }

    present = 1;

    mmio_write(V_STATUS, 0);
    memory_barrier();
    mmio_write(V_STATUS, STATUS_ACKNOWLEDGE);
    mmio_write(V_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER);

    mmio_write(V_DEVICE_FEATURES_SEL, 0);
    uint32_t features_lo = mmio_read(V_DEVICE_FEATURES);
    mmio_write(V_DEVICE_FEATURES_SEL, 1);
    uint32_t features_hi = mmio_read(V_DEVICE_FEATURES);

    if ((features_hi & (1U << (VIRTIO_F_VERSION_1 - 32U))) == 0)
    {
        uart_puts("VirtIO Block: VIRTIO_F_VERSION_1 missing.\r\n");
        fail_device();
        return;
    }

    negotiated_features_low = 0;
    if (features_lo & (1U << VIRTIO_BLK_F_FLUSH))
        negotiated_features_low |= (1U << VIRTIO_BLK_F_FLUSH);

    negotiated_features_high = (1U << (VIRTIO_F_VERSION_1 - 32U));

    if (features_lo & (1U << VIRTIO_BLK_F_RO))
        read_only = 1;

    mmio_write(V_DRIVER_FEATURES_SEL, 0);
    mmio_write(V_DRIVER_FEATURES, negotiated_features_low);
    mmio_write(V_DRIVER_FEATURES_SEL, 1);
    mmio_write(V_DRIVER_FEATURES, negotiated_features_high);

    mmio_write(V_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER | STATUS_FEATURES_OK);
    memory_barrier();

    if ((mmio_read(V_STATUS) & STATUS_FEATURES_OK) == 0)
    {
        uart_puts("VirtIO Block: feature negotiation rejected.\r\n");
        fail_device();
        return;
    }

    if (!init_queue())
    {
        uart_puts("VirtIO Block: queue initialization failed.\r\n");
        fail_device();
        return;
    }

    /* Read 64-bit capacity consistently through the MMIO config-generation field. */
    uint32_t gen_before;
    uint32_t gen_after;
    do
    {
        gen_before = mmio_read(V_CONFIG_GENERATION);
        capacity_sectors = config_u64(V_CONFIG + 0x00);
        uint32_t blk = mmio_read(V_CONFIG + 0x18);
        gen_after = mmio_read(V_CONFIG_GENERATION);
        if (gen_before != gen_after)
            capacity_sectors = 0;
        if (blk != 0)
            logical_block_size = blk;
    } while (gen_before != gen_after);

    if (capacity_sectors == 0)
    {
        uart_puts("VirtIO Block: device reports zero capacity.\r\n");
        fail_device();
        return;
    }

    mmio_write(V_STATUS,
        STATUS_ACKNOWLEDGE |
        STATUS_DRIVER |
        STATUS_FEATURES_OK |
        STATUS_DRIVER_OK);
    memory_barrier();

    if ((mmio_read(V_STATUS) & STATUS_DRIVER_OK) == 0)
    {
        uart_puts("VirtIO Block: device refused DRIVER_OK.\r\n");
        fail_device();
        return;
    }

    ready = 1;

    uart_puts("VirtIO Block: ready.\r\n");
}

int virtio_blk_present()
{
    return present;
}

int virtio_blk_ready()
{
    return ready;
}

int virtio_blk_read(uint64_t sector, void* buffer)
{
    if (buffer == 0)
        return -22;
    return submit_request(REQ_IN, sector, 0, (uint8_t*)buffer);
}

int virtio_blk_write(uint64_t sector, const void* buffer)
{
    if (buffer == 0)
        return -22;
    if (read_only)
        return -30;
    return submit_request(REQ_OUT, sector, (const uint8_t*)buffer, 0);
}

int virtio_blk_flush()
{
    if (!ready)
        return -19;
    if ((negotiated_features_low & (1U << VIRTIO_BLK_F_FLUSH)) == 0)
        return 0;
    return submit_request(REQ_FLUSH, 0, 0, 0);
}

uint64_t virtio_blk_capacity()
{
    return capacity_sectors;
}

uint32_t virtio_blk_block_size()
{
    return logical_block_size;
}

int virtio_blk_read_only()
{
    return read_only;
}

uint64_t virtio_blk_base()
{
    return device_base;
}

int virtio_blk_irq()
{
    return device_irq;
}

static void print_u64(uint64_t value)
{
    char b[32];
    int p = 0;
    if (value == 0)
    {
        uart_putc('0');
        return;
    }
    while (value && p < 31)
    {
        b[p++] = (char)('0' + (value % 10ULL));
        value /= 10ULL;
    }
    while (p)
        uart_putc(b[--p]);
}

static void print_hex8(uint8_t value)
{
    static const char d[] = "0123456789ABCDEF";
    uart_putc(d[(value >> 4) & 0xF]);
    uart_putc(d[value & 0xF]);
}

void virtio_blk_status()
{
    uart_puts("VirtIO Block status\r\n");
    uart_puts("-------------------\r\n");
    uart_puts("Present  : "); uart_puts(present ? "yes\r\n" : "no\r\n");
    uart_puts("Ready    : "); uart_puts(ready ? "yes\r\n" : "no\r\n");
    uart_puts("Read-only: "); uart_puts(read_only ? "yes\r\n" : "no\r\n");
    uart_puts("Base     : 0x");
    static const char d[] = "0123456789ABCDEF";
    for (int s = 60; s >= 0; s -= 4) uart_putc(d[(device_base >> s) & 0xF]);
    uart_puts("\r\nIRQ      : ");
    if (device_irq < 0) uart_puts("unknown\r\n"); else { print_u64((uint64_t)device_irq); uart_puts("\r\n"); }
    uart_puts("Sectors  : "); print_u64(capacity_sectors); uart_puts("\r\n");
    uart_puts("Size     : "); print_u64(capacity_sectors * (uint64_t)SECTOR_SIZE / (1024ULL * 1024ULL)); uart_puts(" MiB\r\n");
    uart_puts("Block    : "); print_u64(logical_block_size); uart_puts(" bytes\r\n");
    uart_puts("Queue    : "); print_u64((uint64_t)queue_size); uart_puts(" entries\r\n");

    if (ready)
    {
        uint8_t test[SECTOR_SIZE];
        int rc = virtio_blk_read(0, test);
        uart_puts("Probe    : ");
        if (rc == 0)
        {
            uart_puts("sector 0 read OK\r\n");
            uart_puts("Hex      : ");
            for (int i = 0; i < 16; ++i) { print_hex8(test[i]); uart_putc(' '); }
            uart_puts("\r\n");
        }
        else
        {
            uart_puts("FAILED (");
            if (rc < 0) { uart_putc('-'); rc = -rc; }
            print_u64((uint64_t)rc);
            uart_puts(")\r\n");
        }
    }
}
