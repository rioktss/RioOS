#include "virtio_input.h"
#include "virtio_mmio.h"
#include "uart.h"

#define V_DEVICE_ID          0x008U
#define V_DEVICE_FEATURES    0x010U
#define V_DEVICE_FEATURES_SEL 0x014U
#define V_DRIVER_FEATURES    0x020U
#define V_DRIVER_FEATURES_SEL 0x024U
#define V_QUEUE_SEL           0x030U
#define V_QUEUE_NUM_MAX       0x034U
#define V_QUEUE_NUM           0x038U
#define V_QUEUE_READY         0x044U
#define V_QUEUE_NOTIFY        0x050U
#define V_STATUS              0x070U
#define V_QUEUE_DESC_LOW      0x080U
#define V_QUEUE_DESC_HIGH     0x084U
#define V_QUEUE_DRIVER_LOW    0x090U
#define V_QUEUE_DRIVER_HIGH   0x094U
#define V_QUEUE_DEVICE_LOW    0x0A0U
#define V_QUEUE_DEVICE_HIGH   0x0A4U
#define V_DEVICE_CONFIG       0x100U
#define VIRTIO_INPUT_ID       18U
#define VIRTIO_F_VERSION_1    32U
#define STATUS_ACK            0x01U
#define STATUS_DRIVER         0x02U
#define STATUS_DRIVER_OK      0x04U
#define STATUS_FEATURES_OK    0x08U
#define STATUS_FAILED         0x80U
#define DESC_WRITE            2U
#define QUEUE_COUNT           8U
#define INPUT_EV_SYN          0U
#define INPUT_EV_KEY          1U
#define INPUT_EV_ABS          3U
#define INPUT_CFG_ID_NAME     1U
#define INPUT_CFG_ABS_INFO    0x12U
#define INPUT_SYN_REPORT      0U
#define INPUT_ABS_X           0U
#define INPUT_ABS_Y           1U
#define INPUT_BTN_LEFT        272U
#define INPUT_BTN_TOUCH       330U

struct VirtqDesc { uint64_t addr; uint32_t len; uint16_t flags; uint16_t next; };
struct VirtqAvail { uint16_t flags; uint16_t idx; uint16_t ring[QUEUE_COUNT]; uint16_t used_event; };
struct VirtqUsedElem { uint32_t id; uint32_t len; };
struct VirtqUsed { uint16_t flags; volatile uint16_t idx; VirtqUsedElem ring[QUEUE_COUNT]; uint16_t avail_event; };
struct VirtioInputEvent { uint16_t type; uint16_t code; int32_t value; };

struct InputDevice
{
    volatile uint32_t* regs;
    uint64_t base;
    int ready;
    int is_pointer;
    uint16_t last_used;
    uint16_t queue_size;
    int32_t pointer_x;
    int32_t pointer_y;
    int32_t abs_min_x;
    int32_t abs_max_x;
    int32_t abs_min_y;
    int32_t abs_max_y;
    char name[64];
    uint32_t pointer_down;
    uint32_t pointer_dirty;
};

static InputDevice devices[2];
static VirtqDesc event_desc[2][QUEUE_COUNT] __attribute__((aligned(16), section(".bss.virtio_input")));
static VirtqAvail event_avail[2] __attribute__((aligned(16), section(".bss.virtio_input")));
static VirtqUsed event_used[2] __attribute__((aligned(16), section(".bss.virtio_input")));
static VirtioInputEvent event_buffers[2][QUEUE_COUNT] __attribute__((aligned(16), section(".bss.virtio_input")));
static VirtqDesc status_desc[2][1] __attribute__((aligned(16), section(".bss.virtio_input")));
static VirtqAvail status_avail[2] __attribute__((aligned(16), section(".bss.virtio_input")));
static VirtqUsed status_used[2] __attribute__((aligned(16), section(".bss.virtio_input")));
static uint32_t shift_down = 0;
static uint32_t ctrl_down = 0;
static uint32_t next_device = 0;

static void io_barrier() { asm volatile("dmb osh\n dsb sy" ::: "memory"); }
static uint32_t reg_read(InputDevice* d, uint32_t offset) { return d->regs[offset / 4U]; }
static void reg_write(InputDevice* d, uint32_t offset, uint32_t value) { d->regs[offset / 4U] = value; }

static volatile uint8_t* config_bytes(InputDevice* d)
{
    return (volatile uint8_t*)(d->base + V_DEVICE_CONFIG);
}

static uint32_t config_read_u32(InputDevice* d, uint8_t select, uint8_t subselect,
                                uint32_t data_offset, uint32_t* data_size)
{
    volatile uint8_t* cfg = config_bytes(d);
    cfg[0] = select;
    cfg[1] = subselect;
    io_barrier();
    uint32_t size = cfg[2];
    if (data_size) *data_size = size;
    if (size < data_offset + 4U) return 0;
    uint32_t value = 0;
    for (uint32_t i = 0; i < 4U; ++i) value |= (uint32_t)cfg[8U + data_offset + i] << (i * 8U);
    return value;
}

static int name_contains(const char* text, const char* needle)
{
    if (!text || !needle || !needle[0]) return 0;
    for (uint32_t i = 0; text[i]; ++i)
    {
        uint32_t j = 0;
        while (needle[j] && text[i + j] && text[i + j] == needle[j]) ++j;
        if (!needle[j]) return 1;
    }
    return 0;
}

static void read_device_config(InputDevice* d, uint32_t fallback_index)
{
    volatile uint8_t* cfg = config_bytes(d);
    cfg[0] = INPUT_CFG_ID_NAME;
    cfg[1] = 0;
    io_barrier();
    uint32_t name_size = cfg[2];
    if (name_size >= sizeof(d->name)) name_size = sizeof(d->name) - 1U;
    for (uint32_t i = 0; i < name_size; ++i) d->name[i] = (char)cfg[8U + i];
    d->name[name_size] = '\0';

    /* Device order varies between QEMU releases; use its advertised name. */
    if (name_contains(d->name, "Keyboard")) d->is_pointer = 0;
    else if (name_contains(d->name, "Tablet") || name_contains(d->name, "Mouse")) d->is_pointer = 1;
    else d->is_pointer = (fallback_index == 0U) ? 1 : 0;

    d->abs_min_x = 0; d->abs_max_x = 32767;
    d->abs_min_y = 0; d->abs_max_y = 32767;
    if (d->is_pointer)
    {
        uint32_t size = 0;
        uint32_t min_x = config_read_u32(d, INPUT_CFG_ABS_INFO, INPUT_ABS_X, 0, &size);
        uint32_t max_x = config_read_u32(d, INPUT_CFG_ABS_INFO, INPUT_ABS_X, 4, &size);
        if (size >= 8U && (int32_t)max_x > (int32_t)min_x)
        { d->abs_min_x = (int32_t)min_x; d->abs_max_x = (int32_t)max_x; }
        uint32_t min_y = config_read_u32(d, INPUT_CFG_ABS_INFO, INPUT_ABS_Y, 0, &size);
        uint32_t max_y = config_read_u32(d, INPUT_CFG_ABS_INFO, INPUT_ABS_Y, 4, &size);
        if (size >= 8U && (int32_t)max_y > (int32_t)min_y)
        { d->abs_min_y = (int32_t)min_y; d->abs_max_y = (int32_t)max_y; }
    }
}

static int32_t normalize_axis(int32_t value, int32_t minimum, int32_t maximum)
{
    if (maximum <= minimum) return 0;
    if (value <= minimum) return 0;
    if (value >= maximum) return 32767;
    uint64_t numerator = (uint64_t)(value - minimum) * 32767ULL;
    return (int32_t)(numerator / (uint32_t)(maximum - minimum));
}

static void zero_queue(VirtqDesc* desc, VirtqAvail* avail, VirtqUsed* used, uint32_t n)
{
    for (uint32_t i = 0; i < n; ++i)
    {
        desc[i].addr = 0; desc[i].len = 0; desc[i].flags = 0; desc[i].next = 0;
        avail->ring[i] = 0;
        used->ring[i].id = 0; used->ring[i].len = 0;
    }
    avail->flags = 0; avail->idx = 0; avail->used_event = 0;
    used->flags = 0; used->idx = 0; used->avail_event = 0;
}

static int setup_queue(InputDevice* d, uint32_t device_index, uint16_t queue_index,
                       VirtqDesc* desc, VirtqAvail* avail, VirtqUsed* used,
                       uint32_t requested, uint32_t local_max, uint16_t* actual_size)
{
    reg_write(d, V_QUEUE_SEL, queue_index);
    uint32_t max = reg_read(d, V_QUEUE_NUM_MAX);
    if (max == 0 || reg_read(d, V_QUEUE_READY) != 0) return 0;
    uint32_t n = max < requested ? max : requested;
    if (n > local_max) n = local_max;
    if (n == 0) return 0;
    zero_queue(desc, avail, used, n);
    reg_write(d, V_QUEUE_NUM, n);
    uint64_t dp = (uint64_t)desc, ap = (uint64_t)avail, up = (uint64_t)used;
    reg_write(d, V_QUEUE_DESC_LOW, (uint32_t)dp); reg_write(d, V_QUEUE_DESC_HIGH, (uint32_t)(dp >> 32));
    reg_write(d, V_QUEUE_DRIVER_LOW, (uint32_t)ap); reg_write(d, V_QUEUE_DRIVER_HIGH, (uint32_t)(ap >> 32));
    reg_write(d, V_QUEUE_DEVICE_LOW, (uint32_t)up); reg_write(d, V_QUEUE_DEVICE_HIGH, (uint32_t)(up >> 32));
    io_barrier();
    (void)device_index;
    if (actual_size) *actual_size = (uint16_t)n;
    /* The caller fills descriptors/avail before setting QueueReady. */
    return 1;
}

static int init_one(uint32_t index)
{
    InputDevice* d = &devices[index];
    uint64_t size = 0; int irq = -1;
    d->ready = 0; d->regs = 0; d->base = 0; d->last_used = 0; d->queue_size = 0;
    d->is_pointer = 0;
    d->pointer_x = 16384; d->pointer_y = 16384; d->pointer_down = 0; d->pointer_dirty = 0;
    d->name[0] = '\0';
    if (!virtio_mmio_find_device_instance(VIRTIO_INPUT_ID, index, &d->base, &size, &irq)) return 0;
    (void)size; (void)irq;
    d->regs = (volatile uint32_t*)d->base;
    if (d->regs[0] != 0x74726976U || d->regs[1] != 2U || reg_read(d, V_DEVICE_ID) != VIRTIO_INPUT_ID)
    { d->regs = 0; return 0; }

    read_device_config(d, index);
    reg_write(d, V_STATUS, 0);
    reg_write(d, V_STATUS, STATUS_ACK);
    reg_write(d, V_STATUS, STATUS_ACK | STATUS_DRIVER);
    reg_write(d, V_DEVICE_FEATURES_SEL, 0);
    uint32_t features_lo = reg_read(d, V_DEVICE_FEATURES);
    reg_write(d, V_DEVICE_FEATURES_SEL, 1);
    uint32_t features_hi = reg_read(d, V_DEVICE_FEATURES);
    (void)features_lo;
    if ((features_hi & (1U << (VIRTIO_F_VERSION_1 - 32U))) == 0)
    {
        reg_write(d, V_STATUS, STATUS_ACK | STATUS_DRIVER | STATUS_FAILED); return 0;
    }
    reg_write(d, V_DRIVER_FEATURES_SEL, 0); reg_write(d, V_DRIVER_FEATURES, 0);
    reg_write(d, V_DRIVER_FEATURES_SEL, 1); reg_write(d, V_DRIVER_FEATURES, 1U);
    reg_write(d, V_STATUS, STATUS_ACK | STATUS_DRIVER | STATUS_FEATURES_OK);
    io_barrier();
    if ((reg_read(d, V_STATUS) & STATUS_FEATURES_OK) == 0) return 0;

    if (!setup_queue(d, index, 0, event_desc[index], &event_avail[index], &event_used[index], QUEUE_COUNT, QUEUE_COUNT, &d->queue_size) ||
        !setup_queue(d, index, 1, status_desc[index], &status_avail[index], &status_used[index], 1, 1, 0))
    {
        reg_write(d, V_STATUS, reg_read(d, V_STATUS) | STATUS_FAILED); return 0;
    }
    if (d->queue_size == 0 || d->queue_size > QUEUE_COUNT)
    {
        reg_write(d, V_STATUS, reg_read(d, V_STATUS) | STATUS_FAILED); return 0;
    }
    for (uint32_t i = 0; i < d->queue_size; ++i)
    {
        event_desc[index][i].addr = (uint64_t)&event_buffers[index][i];
        event_desc[index][i].len = sizeof(VirtioInputEvent);
        event_desc[index][i].flags = DESC_WRITE;
        event_desc[index][i].next = 0;
        event_avail[index].ring[i] = (uint16_t)i;
    }
    io_barrier();
    event_avail[index].idx = d->queue_size;
    io_barrier();
    reg_write(d, V_QUEUE_SEL, 0);
    reg_write(d, V_QUEUE_READY, 1);
    if (reg_read(d, V_QUEUE_READY) != 1)
    { reg_write(d, V_STATUS, reg_read(d, V_STATUS) | STATUS_FAILED); return 0; }
    reg_write(d, V_QUEUE_SEL, 1);
    reg_write(d, V_QUEUE_READY, 1);
    if (reg_read(d, V_QUEUE_READY) != 1)
    { reg_write(d, V_STATUS, reg_read(d, V_STATUS) | STATUS_FAILED); return 0; }
    io_barrier();
    reg_write(d, V_STATUS, STATUS_ACK | STATUS_DRIVER | STATUS_FEATURES_OK | STATUS_DRIVER_OK);
    io_barrier();
    if ((reg_read(d, V_STATUS) & STATUS_DRIVER_OK) == 0)
    { reg_write(d, V_STATUS, reg_read(d, V_STATUS) | STATUS_FAILED); return 0; }
    reg_write(d, V_QUEUE_SEL, 0);
    reg_write(d, V_QUEUE_NOTIFY, 0);
    d->ready = 1;
    return 1;
}

static char translate_key(uint16_t code, int shift)
{
    if (code >= 16 && code <= 25) { static const char row[] = "qwertyuiop"; char c = row[code - 16]; return shift ? (char)(c - 'a' + 'A') : c; }
    if (code >= 30 && code <= 38) { static const char row[] = "asdfghjkl"; char c = row[code - 30]; return shift ? (char)(c - 'a' + 'A') : c; }
    if (code >= 44 && code <= 50) { static const char row[] = "zxcvbnm"; char c = row[code - 44]; return shift ? (char)(c - 'a' + 'A') : c; }
    if (code >= 2 && code <= 11)
    {
        static const char normal[] = "1234567890";
        static const char shifted[] = "!@#$%^&*()";
        return shift ? shifted[code - 2] : normal[code - 2];
    }
    switch (code)
    {
        case 1: return 27; case 14: return 8; case 15: return '\t'; case 28: return '\n'; case 57: return ' ';
        case 12: return shift ? '_' : '-'; case 13: return shift ? '+' : '=';
        case 26: return shift ? '{' : '['; case 27: return shift ? '}' : ']';
        case 39: return shift ? ':' : ';'; case 40: return shift ? '"' : '\'';
        case 41: return shift ? '~' : '`'; case 43: return shift ? '|' : '\\';
        case 51: return shift ? '<' : ','; case 52: return shift ? '>' : '.'; case 53: return shift ? '?' : '/';
        case 103: return (char)0x80; case 108: return (char)0x81;
        case 105: return (char)0x82; case 106: return (char)0x83;
        default: return 0;
    }
}

static int event_pop(uint32_t i, struct VirtioInputEvent* out)
{
    InputDevice* d = &devices[i];
    if (!d->ready || d->queue_size == 0 || event_used[i].idx == d->last_used) return 0;
    io_barrier();
    uint16_t slot = (uint16_t)(d->last_used % d->queue_size);
    uint32_t id = event_used[i].ring[slot].id;
    if (id >= d->queue_size) { d->last_used++; return 0; }
    *out = event_buffers[i][id];
    event_avail[i].ring[event_avail[i].idx % d->queue_size] = (uint16_t)id;
    io_barrier();
    event_avail[i].idx = (uint16_t)(event_avail[i].idx + 1U);
    d->last_used = (uint16_t)(d->last_used + 1U);
    io_barrier();
    reg_write(d, V_QUEUE_SEL, 0); reg_write(d, V_QUEUE_NOTIFY, 0);
    return 1;
}

void virtio_input_init()
{
    for (uint32_t i = 0; i < 2; ++i) devices[i].ready = 0;
    shift_down = 0; ctrl_down = 0; next_device = 0;
    int found = 0;
    for (uint32_t i = 0; i < 2; ++i) found += init_one(i);

    int keys = virtio_input_keyboard_ready();
    int pointer = virtio_input_pointer_ready();
    if (keys) uart_puts("VirtIO input: keyboard ready.\r\n");
    else uart_puts("VirtIO input: keyboard unavailable (serial fallback remains).\r\n");
    if (pointer) uart_puts("VirtIO input: tablet/pointer ready.\r\n");
    else uart_puts("VirtIO input: tablet unavailable (keyboard input remains).\r\n");
    if (found && !keys && !pointer) uart_puts("VirtIO input: device identity was not recognized; check QEMU VirtIO input models.\r\n");
}

int virtio_input_ready()
{
    return devices[0].ready || devices[1].ready;
}

int virtio_input_keyboard_ready()
{
    for (uint32_t i = 0; i < 2; ++i)
        if (devices[i].ready && !devices[i].is_pointer) return 1;
    return 0;
}

int virtio_input_pointer_ready()
{
    for (uint32_t i = 0; i < 2; ++i)
        if (devices[i].ready && devices[i].is_pointer) return 1;
    return 0;
}

int virtio_input_poll(struct RioInputEvent* event)
{
    if (!event) return 0;
    event->kind = RIO_INPUT_NONE; event->key = 0; event->x = 0; event->y = 0; event->pressed = 0;
    for (uint32_t attempt = 0; attempt < 32U; ++attempt)
    {
        uint32_t i = next_device++ & 1U;
        if (!devices[i].ready) continue;
        struct VirtioInputEvent raw;
        if (!event_pop(i, &raw)) continue;
        InputDevice* device = &devices[i];
        if (!device->is_pointer)
        {
            if (raw.type != INPUT_EV_KEY) continue;
            if (raw.code == 42U || raw.code == 54U) { shift_down = raw.value != 0 ? 1U : 0U; continue; }
            if (raw.code == 29U || raw.code == 97U) { ctrl_down = raw.value != 0 ? 1U : 0U; continue; }
            if (raw.value == 0) continue;
            char key = translate_key(raw.code, shift_down != 0);
            if (key == 0) continue;
            if (ctrl_down && key >= 'a' && key <= 'z') key = (char)(key - 'a' + 1);
            else if (ctrl_down && key >= 'A' && key <= 'Z') key = (char)(key - 'A' + 1);
            event->kind = RIO_INPUT_KEY; event->key = key;
            return 1;
        }
        if (!device->is_pointer) continue;
        if (raw.type == INPUT_EV_ABS)
        {
            if (raw.code == INPUT_ABS_X) { device->pointer_x = normalize_axis(raw.value, device->abs_min_x, device->abs_max_x); device->pointer_dirty = 1; }
            else if (raw.code == INPUT_ABS_Y) { device->pointer_y = normalize_axis(raw.value, device->abs_min_y, device->abs_max_y); device->pointer_dirty = 1; }
        }
        else if (raw.type == INPUT_EV_KEY && (raw.code == INPUT_BTN_LEFT || raw.code == INPUT_BTN_TOUCH))
        {
            device->pointer_down = raw.value != 0 ? 1U : 0U;
            device->pointer_dirty = 1;
        }
        else if (raw.type == INPUT_EV_SYN && raw.code == INPUT_SYN_REPORT && device->pointer_dirty)
        {
            event->kind = RIO_INPUT_POINTER;
            event->x = device->pointer_x; event->y = device->pointer_y;
            event->pressed = device->pointer_down;
            device->pointer_dirty = 0;
            return 1;
        }
    }
    return 0;
}

int virtio_input_try_get_key(char* key)
{
    if (!key) return 0;
    struct RioInputEvent event;
    for (uint32_t i = 0; i < 32U; ++i)
    {
        if (!virtio_input_poll(&event)) return 0;
        if (event.kind == RIO_INPUT_KEY) { *key = event.key; return 1; }
    }
    return 0;
}
