#include "fb.h"
#ifndef HOST_TEST
#include "uart.h"
#endif

#define FB_DEFAULT_WIDTH  1024
#define FB_DEFAULT_HEIGHT 768
#define FB_BACKGROUND     0x00000000U
#define FW_CFG_BASE       0x09020000ULL
#define FW_CFG_SIGNATURE  0x0000U
#define FW_CFG_ID         0x0001U
#define FW_CFG_FILE_DIR   0x0019U
#define FW_CFG_DMA_SELECT 0x00000008U
#define FW_CFG_DMA_WRITE  0x00000010U
#define FW_CFG_DMA_READ   0x00000002U
#define RAMFB_FOURCC_XRGB8888 0x34325258U

#ifndef HOST_TEST
static uint32_t ramfb_pixels[FB_DEFAULT_WIDTH * FB_DEFAULT_HEIGHT]
    __attribute__((aligned(4096), section(".bss.ramfb")));
#endif
static uint32_t* fb_base = 0;
static int fb_w = FB_DEFAULT_WIDTH;
static int fb_h = FB_DEFAULT_HEIGHT;
static int fb_ready = 0;
static int fb_display_ready = 0;

#ifndef HOST_TEST
struct FwCfgDma
{
    uint32_t control;
    uint32_t length;
    uint64_t address;
} __attribute__((packed, aligned(16)));

struct RamfbConfig
{
    uint64_t address;
    uint32_t fourcc;
    uint32_t flags;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
} __attribute__((packed));

/* QEMU's etc/ramfb fw_cfg item is exactly 28 bytes. Keep the data structure
   packed to that wire size, and align the object separately for DMA/cache use.
   Applying aligned(16) to the type itself would round sizeof(RamfbConfig) up
   to 32 bytes, making QEMU reject the otherwise-correct DMA write. */
static_assert(sizeof(RamfbConfig) == 28, "QEMU ramfb config must be exactly 28 bytes");

static FwCfgDma fwcfg_dma __attribute__((aligned(16)));
static RamfbConfig ramfb_cfg __attribute__((aligned(16)));

static uint16_t swap16(uint16_t x)
{
    return (uint16_t)((x >> 8) | (x << 8));
}

static uint32_t swap32(uint32_t x)
{
    return ((x & 0x000000FFU) << 24) | ((x & 0x0000FF00U) << 8) |
           ((x & 0x00FF0000U) >> 8) | ((x & 0xFF000000U) >> 24);
}

static uint64_t swap64(uint64_t x)
{
    return ((uint64_t)swap32((uint32_t)x) << 32) | swap32((uint32_t)(x >> 32));
}

static uint32_t read_be32(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint16_t read_be16(const uint8_t* p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

/* fw_cfg exposes the feature word (FW_CFG_ID) as a little-endian value,
   while the file directory and the DMA registers are big-endian. */
static uint32_t read_le32(const uint8_t* p)
{
    return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[1] << 8) | (uint32_t)p[0];
}

static const char* ramfb_fail_reason = "unknown";

static void cache_clean(const void* ptr, uint64_t bytes)
{
    uint64_t start = (uint64_t)ptr & ~63ULL;
    uint64_t end = ((uint64_t)ptr + bytes + 63ULL) & ~63ULL;
    for (uint64_t p = start; p < end; p += 64ULL)
        asm volatile("dc cvac, %0" :: "r"(p) : "memory");
    asm volatile("dsb sy" ::: "memory");
}
static void cache_invalidate(const void* ptr, uint64_t bytes)
{
    uint64_t start = (uint64_t)ptr & ~63ULL;
    uint64_t end = ((uint64_t)ptr + bytes + 63ULL) & ~63ULL;
    for (uint64_t p = start; p < end; p += 64ULL)
        asm volatile("dc ivac, %0" :: "r"(p) : "memory");
    asm volatile("dsb sy" ::: "memory");
}

static void fwcfg_select(uint16_t selector)
{
    volatile uint16_t* reg = (volatile uint16_t*)(FW_CFG_BASE + 8ULL);
    *reg = swap16(selector); /* MMIO selector is big-endian. */
    asm volatile("dsb sy" ::: "memory");
}

static void fwcfg_read_next(uint8_t* output, uint32_t length)
{
    volatile uint8_t* data = (volatile uint8_t*)FW_CFG_BASE;
    for (uint32_t i = 0; i < length; ++i)
        output[i] = data[0];
}

static int fwcfg_dma_transfer(uint32_t control, uint32_t length, const void* address)
{
    fwcfg_dma.control = swap32(control);
    fwcfg_dma.length = swap32(length);
    fwcfg_dma.address = swap64((uint64_t)address);
    cache_clean(&fwcfg_dma, sizeof(fwcfg_dma));
    if (address != 0 && length != 0)
        cache_clean(address, length);

    volatile uint64_t* dma_reg = (volatile uint64_t*)(FW_CFG_BASE + 16ULL);
    *dma_reg = swap64((uint64_t)&fwcfg_dma);
    asm volatile("dsb sy" ::: "memory");

    /* QEMU currently completes fw_cfg DMA synchronously. Bound the wait anyway. */
    for (uint32_t i = 0; i < 1000000U; ++i)
    {
        cache_invalidate(&fwcfg_dma, sizeof(fwcfg_dma));
        uint32_t state = swap32(fwcfg_dma.control);
        if (state == 0U) return 0;
        if ((state & 1U) != 0U) return -1;
    }
    return -1;
}

static int fwcfg_init_ramfb()
{
    uint8_t sig[4] = {0, 0, 0, 0};
    fwcfg_select(FW_CFG_SIGNATURE);
    fwcfg_read_next(sig, sizeof(sig));
    if (sig[0] != 'Q' || sig[1] != 'E' || sig[2] != 'M' || sig[3] != 'U')
    {
        ramfb_fail_reason = "fw_cfg signature is not 'QEMU' (MMIO base wrong or not mapped)";
        return -1;
    }

    uint8_t id_raw[4] = {0, 0, 0, 0};
    fwcfg_select(FW_CFG_ID);
    fwcfg_read_next(id_raw, sizeof(id_raw));
    if ((read_le32(id_raw) & 2U) == 0U)
    {
        ramfb_fail_reason = "fw_cfg DMA interface not advertised";
        return -1; /* The DMA interface is required for ramfb writes. */
    }

    fwcfg_select(FW_CFG_FILE_DIR);
    uint8_t count_raw[4] = {0, 0, 0, 0};
    fwcfg_read_next(count_raw, sizeof(count_raw));
    uint32_t count = read_be32(count_raw);
    if (count > 1024U)
    {
        ramfb_fail_reason = "fw_cfg file directory looks corrupt";
        return -1;
    }

    uint16_t ramfb_selector = 0;
    for (uint32_t i = 0; i < count; ++i)
    {
        /* FWCfgFile is a fixed 64-byte record: size(4) select(2) reserved(2)
           name[56] (NUL padded). */
        uint8_t entry[64];
        fwcfg_read_next(entry, sizeof(entry));
        uint16_t selector = read_be16(entry + 4);
        const char ramfb_name[] = "etc/ramfb";
        int ramfb_match = 1;
        for (uint32_t c = 0; c < sizeof(ramfb_name); ++c)
            if ((char)entry[8 + c] != ramfb_name[c]) { ramfb_match = 0; break; }
        if (ramfb_match) ramfb_selector = selector;
    }

    if (ramfb_selector == 0)
    {
        ramfb_fail_reason = "etc/ramfb not found (start QEMU with -device ramfb)";
        return -1;
    }

    ramfb_cfg.address = swap64((uint64_t)ramfb_pixels);
    ramfb_cfg.fourcc = swap32(RAMFB_FOURCC_XRGB8888);
    ramfb_cfg.flags = swap32(0U);
    ramfb_cfg.width = swap32(FB_DEFAULT_WIDTH);
    ramfb_cfg.height = swap32(FB_DEFAULT_HEIGHT);
    ramfb_cfg.stride = swap32(FB_DEFAULT_WIDTH * 4U);
    cache_clean(&ramfb_cfg, sizeof(ramfb_cfg));

    uint32_t command = ((uint32_t)ramfb_selector << 16) | FW_CFG_DMA_SELECT | FW_CFG_DMA_WRITE;
    if (fwcfg_dma_transfer(command, sizeof(RamfbConfig), &ramfb_cfg) != 0)
    {
        ramfb_fail_reason = "fw_cfg DMA write of the 28-byte ramfb config failed";
        return -1;
    }
    return 0;
}
#endif

void fb_fill_rect(int x, int y, int width, int height, uint32_t color)
{
    if (!fb_ready || fb_base == 0 || width <= 0 || height <= 0) return;
    if (x < 0) { width += x; x = 0; }
    if (y < 0) { height += y; y = 0; }
    if (x >= fb_w || y >= fb_h) return;
    if (width > fb_w - x) width = fb_w - x;
    if (height > fb_h - y) height = fb_h - y;
    if (width <= 0 || height <= 0) return;

    for (int row = 0; row < height; ++row)
    {
        uint32_t* dst = fb_base + (uint64_t)(y + row) * (uint64_t)fb_w + (uint64_t)x;
        for (int col = 0; col < width; ++col) dst[col] = color;
    }
}

void fb_draw_pixel(int x, int y, uint32_t color)
{
    if (!fb_ready || fb_base == 0 || x < 0 || y < 0 || x >= fb_w || y >= fb_h) return;
    fb_base[(uint64_t)y * (uint64_t)fb_w + (uint64_t)x] = color;
}

void fb_init()
{
#ifdef HOST_TEST
    fb_base = 0;
    fb_ready = 0;
    fb_display_ready = 0;
#else
    fb_base = ramfb_pixels;
    fb_w = FB_DEFAULT_WIDTH;
    fb_h = FB_DEFAULT_HEIGHT;
    fb_ready = 0;
    fb_display_ready = 0;
    if (fwcfg_init_ramfb() == 0)
    {
        fb_ready = 1;
        fb_clear(FB_BACKGROUND);
        fb_display_ready = 1;
        fb_present();
        uart_puts("Framebuffer: QEMU ramfb is ready (1024x768 XRGB8888).\r\n");
    }
    else
    {
        uart_puts("Framebuffer: no QEMU display (");
        uart_puts(ramfb_fail_reason);
        uart_puts("); serial console only.\r\n");
        fb_base = 0;
    }
#endif
}

void fb_clear(uint32_t color)
{
    if (!fb_ready || fb_base == 0) return;
    fb_fill_rect(0, 0, fb_w, fb_h, color);
}

void fb_present_rows(int y0, int y1)
{
#ifndef HOST_TEST
    if (!fb_ready || !fb_display_ready || fb_base == 0) return;
    if (y0 < 0) y0 = 0;
    if (y1 > fb_h) y1 = fb_h;
    if (y1 <= y0) return;
    cache_clean(fb_base + (uint64_t)y0 * (uint64_t)fb_w,
                (uint64_t)(y1 - y0) * (uint64_t)fb_w * sizeof(uint32_t));
#else
    (void)y0; (void)y1;
#endif
}

void fb_present()
{
    fb_present_rows(0, fb_h);
}

uint32_t* fb_buffer() { return fb_ready ? fb_base : 0; }
int fb_stride_pixels() { return fb_w; }
int fb_get_width() { return fb_w; }
int fb_get_height() { return fb_h; }
int fb_is_ready() { return fb_ready && fb_base != 0; }
int fb_has_display() { return fb_display_ready; }

#ifdef HOST_TEST
int fb_test_attach(uint32_t* pixels, int width, int height)
{
    if (pixels == 0 || width < 16 || height < 16 || width > 4096 || height > 4096)
    {
        fb_base = 0; fb_ready = 0; fb_display_ready = 0; return -1;
    }
    fb_base = pixels; fb_w = width; fb_h = height;
    fb_ready = 1; fb_display_ready = 1;
    fb_clear(FB_BACKGROUND);
    return 0;
}
uint32_t fb_test_pixel(int x, int y)
{
    if (!fb_ready || fb_base == 0 || x < 0 || y < 0 || x >= fb_w || y >= fb_h) return 0;
    return fb_base[(uint64_t)y * (uint64_t)fb_w + (uint64_t)x];
}
#endif
