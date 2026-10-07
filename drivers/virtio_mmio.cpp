#include "virtio_mmio.h"
#include "uart.h"

/* Physical address of the device tree blob, as passed by QEMU/the boot
   loader in x0 at kernel entry and saved by boot.S. This must NOT be a
   hardcoded RAM address: the kernel image itself is linked to load at
   the start of RAM, so a fixed guess (e.g. RAM_START) reads the
   kernel's own code as a fake FDT header and always fails to match
   FDT_MAGIC, silently breaking every virtio-mmio device lookup. */
extern "C" uint64_t boot_dtb_ptr;
uint64_t boot_dtb_ptr = 0;
#define FDT_MAGIC         0xD00DFEEDU
#define FDT_BEGIN_NODE    1U
#define FDT_END_NODE      2U
#define FDT_PROP          3U
#define FDT_NOP           4U
#define FDT_END            9U
#define MAX_FDT_SCAN      (1024U * 1024U)
#define MMIO_DEVICE_MIN   0x0A000000ULL
#define MMIO_DEVICE_MAX   0x0A200000ULL
#define V_DEVICE_ID       0x008U
#define V_MAGIC           0x000U
#define V_VERSION         0x004U
#define VIRTIO_MAGIC      0x74726976U
#define VIRTIO_VERSION    2U

static uint32_t be32(const void* p)
{
    const uint8_t* b = (const uint8_t*)p;
    return ((uint32_t)b[0] << 24) |
           ((uint32_t)b[1] << 16) |
           ((uint32_t)b[2] << 8) |
           (uint32_t)b[3];
}

static uint64_t be64(const void* p)
{
    return ((uint64_t)be32(p) << 32) | be32((const uint8_t*)p + 4);
}

static uint32_t align4(uint32_t v)
{
    return (v + 3U) & ~3U;
}

static int name_is_compatible(const char* name)
{
    if (name == 0) return 0;
    const char* s = "compatible";
    for (int i = 0; s[i]; ++i)
        if (name[i] != s[i]) return 0;
    return name[10] == '\0';
}

static int name_is_reg(const char* name)
{
    return name && name[0] == 'r' && name[1] == 'e' && name[2] == 'g' && name[3] == '\0';
}

static int compatible_has_virtio_mmio(const uint8_t* value, uint32_t len)
{
    uint32_t pos = 0;
    while (pos < len)
    {
        uint32_t left = len - pos;
        uint32_t n = 0;
        while (n < left && value[pos + n] != 0) ++n;
        if (n >= 11 &&
            value[pos + 0] == 'v' && value[pos + 1] == 'i' && value[pos + 2] == 'r' &&
            value[pos + 3] == 't' && value[pos + 4] == 'i' && value[pos + 5] == 'o' &&
            value[pos + 6] == ',' && value[pos + 7] == 'm' && value[pos + 8] == 'm' &&
            value[pos + 9] == 'i' && value[pos + 10] == 'o')
            return 1;
        if (n == left) break;
        pos += n + 1;
    }
    return 0;
}

/* Fallback used when no DTB is available: QEMU's "virt" machine always
   maps its virtio-mmio transports at a fixed, well-known physical
   window (32 slots of 0x200 bytes starting at 0x0A000000) regardless of
   whether a DTB was handed to the guest, so we can find devices by
   probing each slot's magic/version/device-id registers directly. */
static int virtio_mmio_scan_fixed_window(uint32_t wanted_device_id,
                                          uint64_t* base,
                                          uint64_t* size,
                                          int* irq)
{
    for (uint64_t addr = MMIO_DEVICE_MIN; addr < MMIO_DEVICE_MAX; addr += 0x200ULL)
    {
        volatile uint32_t* regs = (volatile uint32_t*)addr;
        if (regs[V_MAGIC / 4] == VIRTIO_MAGIC &&
            regs[V_VERSION / 4] == VIRTIO_VERSION &&
            regs[V_DEVICE_ID / 4] == wanted_device_id)
        {
            *base = addr;
            *size = 0x200ULL;
            *irq = -1;
            return 1;
        }
    }
    return 0;
}

int virtio_mmio_find_device(uint32_t wanted_device_id,
                            uint64_t* base,
                            uint64_t* size,
                            int* irq)
{
    if (!base || !size || !irq) return 0;
    *irq = -1;

    if (boot_dtb_ptr == 0)
        return virtio_mmio_scan_fixed_window(wanted_device_id, base, size, irq);

    const uint8_t* dtb = (const uint8_t*)boot_dtb_ptr;
    if (be32(dtb) != FDT_MAGIC)
        return virtio_mmio_scan_fixed_window(wanted_device_id, base, size, irq);

    uint32_t total = be32(dtb + 4);
    uint32_t off_struct = be32(dtb + 8);
    uint32_t off_strings = be32(dtb + 12);
    uint32_t size_strings = be32(dtb + 32);
    uint32_t size_struct = be32(dtb + 36);
    if (total < 40 || total > MAX_FDT_SCAN) return 0;
    if (off_struct >= total || off_strings >= total) return 0;
    if (size_struct > total - off_struct || size_strings > total - off_strings) return 0;

    const uint8_t* p = dtb + off_struct;
    const uint8_t* end = p + size_struct;
    const char* strings = (const char*)(dtb + off_strings);

    int depth = 0;
    int candidate = 0;
    uint64_t candidate_base = 0;
    uint64_t candidate_size = 0;
    int candidate_irq = -1;

    while (p + 4 <= end)
    {
        uint32_t token = be32(p);
        p += 4;

        if (token == FDT_BEGIN_NODE)
        {
            while (p < end && *p != 0) ++p;
            if (p >= end) return 0;
            ++p;
            while ((((uint64_t)p - (uint64_t)dtb) & 3ULL) != 0) ++p;
            ++depth;
            continue;
        }

        if (token == FDT_END_NODE)
        {
            if (candidate && candidate_base >= MMIO_DEVICE_MIN && candidate_base < MMIO_DEVICE_MAX && candidate_size >= 0x200)
            {
                volatile uint32_t* regs = (volatile uint32_t*)candidate_base;
                if (regs[V_MAGIC / 4] == VIRTIO_MAGIC &&
                    regs[V_VERSION / 4] == VIRTIO_VERSION &&
                    regs[V_DEVICE_ID / 4] == wanted_device_id)
                {
                    *base = candidate_base;
                    *size = candidate_size;
                    *irq = candidate_irq;
                    return 1;
                }
            }
            candidate = 0;
            candidate_base = 0;
            candidate_size = 0;
            candidate_irq = -1;
            if (depth > 0) --depth;
            continue;
        }

        if (token == FDT_NOP) continue;
        if (token == FDT_END) break;
        if (token != FDT_PROP || p + 8 > end) return 0;

        uint32_t len = be32(p);
        uint32_t nameoff = be32(p + 4);
        p += 8;
        if (nameoff >= size_strings || len > (uint32_t)(end - p)) return 0;
        const char* name = strings + nameoff;

        if (name_is_compatible(name))
        {
            if (len > 0 && compatible_has_virtio_mmio(p, len)) candidate = 1;
        }
        else if (candidate && name_is_reg(name) && len >= 16)
        {
            candidate_base = be64(p);
            candidate_size = be64(p + 8);
        }
        else if (candidate && len >= 12 &&
                 name[0] == 'i' && name[1] == 'n' && name[2] == 't' &&
                 name[3] == 'e' && name[4] == 'r' && name[5] == 'r' &&
                 name[6] == 'u' && name[7] == 'p' && name[8] == 't' &&
                 name[9] == 's' && name[10] == '\0')
        {
            candidate_irq = (int)be32(p + 4);
        }

        p += align4(len);
    }
    return virtio_mmio_scan_fixed_window(wanted_device_id, base, size, irq);
}
