#include "mmu.h"
#include "uart.h"

#define RAM_START 0x40000000ULL
#define RAM_SIZE (128ULL * 1024ULL * 1024ULL)
#define L3_ENTRIES 512
#define RAM_L3_TABLES 64
#define MAX_USER_PAGES ((MMU_USER_LIMIT - MMU_USER_BASE) / MMU_PAGE_SIZE)

#define DESC_VALID (1ULL << 0)
#define DESC_TABLE (1ULL << 1)
#define DESC_AF (1ULL << 10)
#define DESC_SH_INNER (3ULL << 8)
#define DESC_SH_OUTER (2ULL << 8)
#define DESC_ATTR_DEVICE (1ULL << 2)
#define DESC_ATTR_NORMAL (0ULL << 2)
#define DESC_AP_EL1_RW (0ULL << 6)
#define DESC_AP_EL0_RW (1ULL << 6)
#define DESC_AP_EL1_RO (2ULL << 6)
#define DESC_AP_EL0_RO (3ULL << 6)
#define DESC_PXN (1ULL << 53)
#define DESC_UXN (1ULL << 54)

#define MAIR_NORMAL_WBWA 0xFFULL
#define MAIR_DEVICE_nGnRE 0x04ULL
#define TCR_T0SZ_39BIT 25ULL
#define TCR_IRGN0_WBWA (1ULL << 8)
#define TCR_ORGN0_WBWA (1ULL << 10)
#define TCR_SH0_INNER (3ULL << 12)
#define TCR_EPD1 (1ULL << 23)
#define TCR_IPS_40BIT (2ULL << 32)
#define SCTLR_M (1ULL << 0)
#define SCTLR_C (1ULL << 2)
#define SCTLR_I (1ULL << 12)

#define USER_R 1U
#define USER_W 2U
#define USER_X 4U

static uint64_t l1_table[512]
    __attribute__((aligned(4096), section(".bss.mmu_tables")));
static uint64_t l2_low[512]
    __attribute__((aligned(4096), section(".bss.mmu_tables")));
static uint64_t l2_ram[512]
    __attribute__((aligned(4096), section(".bss.mmu_tables")));
static uint64_t ram_l3[RAM_L3_TABLES][512]
    __attribute__((aligned(4096), section(".bss.mmu_tables")));

extern "C" char exception_vectors_el1[];
extern "C" char __kernel_exec_start[];
extern "C" char __kernel_exec_end[];

static uint8_t user_page_modes[MAX_USER_PAGES];
static int user_active = 0;

static uint64_t read_sctlr()
{
    uint64_t value;
    asm volatile("mrs %0, sctlr_el1" : "=r"(value));
    return value;
}

static uint64_t read_ttbr0()
{
    uint64_t value;
    asm volatile("mrs %0, ttbr0_el1" : "=r"(value));
    return value;
}

static uint64_t read_tcr()
{
    uint64_t value;
    asm volatile("mrs %0, tcr_el1" : "=r"(value));
    return value;
}

static uint64_t table_descriptor(uint64_t address)
{
    return (address & ~0xFFFULL) | DESC_VALID | DESC_TABLE;
}

static uint64_t page_descriptor(uint64_t address, uint64_t flags, uint64_t share)
{
    return (address & ~0xFFFULL) | DESC_VALID | DESC_AF | share | flags;
}

static uint64_t page_descriptor_l3(uint64_t address, uint64_t flags, uint64_t share)
{
    return (address & ~0xFFFULL) | DESC_VALID | DESC_TABLE | DESC_AF | share | flags;
}

static uint64_t kernel_rw_nx()
{
    return DESC_ATTR_NORMAL | DESC_AP_EL1_RW | DESC_UXN;
}

static uint64_t kernel_rx()
{
    return DESC_ATTR_NORMAL | DESC_AP_EL1_RO;
}

static uint64_t device_rw_nx()
{
    return DESC_ATTR_DEVICE | DESC_AP_EL1_RW | DESC_SH_OUTER | DESC_PXN | DESC_UXN;
}

static uint64_t user_flags(uint32_t mode)
{
    if ((mode & USER_X)!= 0)
    {
        if ((mode & USER_W)!= 0)
            return DESC_ATTR_NORMAL | DESC_AP_EL0_RW | DESC_PXN;
        return DESC_ATTR_NORMAL | DESC_AP_EL0_RO | DESC_PXN;
    }

    if ((mode & USER_W)!= 0)
        return DESC_ATTR_NORMAL | DESC_AP_EL0_RW | DESC_PXN | DESC_UXN;

    return DESC_ATTR_NORMAL | DESC_AP_EL0_RO | DESC_PXN | DESC_UXN;
}

static void zero_tables()
{
    for (int i = 0; i < 512; ++i)
    {
        l1_table[i] = 0;
        l2_low[i] = 0;
        l2_ram[i] = 0;
    }

    for (int t = 0; t < RAM_L3_TABLES; ++t)
        for (int i = 0; i < 512; ++i)
            ram_l3[t][i] = 0;

    for (uint64_t i = 0; i < MAX_USER_PAGES; ++i)
        user_page_modes[i] = 0;
}

static void map_devices()
{
    l1_table[0] = table_descriptor((uint64_t)l2_low);

    const uint64_t bases[] = {
        0x08000000ULL,
        0x08200000ULL,
        0x09000000ULL,
        0x09020000ULL,
        0x0A000000ULL,
        0x0A200000ULL,
        0x0C000000ULL
    };

    for (uint64_t i = 0; i < sizeof(bases)/sizeof(bases[0]); ++i)
    {
        uint64_t address = bases[i];
        uint64_t index = (address >> 21) & 0x1FFULL;
        if(l2_low[index]==0)
            l2_low[index] = page_descriptor(address, device_rw_nx(), DESC_SH_OUTER);
    }
}

static void map_ram()
{
    l1_table[1] = table_descriptor((uint64_t)l2_ram);

    for (int t = 0; t < RAM_L3_TABLES; ++t)
    {
        l2_ram[t] = table_descriptor((uint64_t)ram_l3[t]);
        for (int p = 0; p < L3_ENTRIES; ++p)
        {
            uint64_t address = RAM_START +
                ((uint64_t)t * 512ULL + (uint64_t)p) * MMU_PAGE_SIZE;
            ram_l3[t][p] = page_descriptor_l3(address, kernel_rw_nx(), DESC_SH_INNER);
        }
    }
}

static void set_ram_flags(uint64_t start, uint64_t end, uint64_t flags)
{
    if (end <= start)
        return;

    uint64_t first = start & ~0xFFFULL;
    uint64_t last = (end + MMU_PAGE_SIZE - 1ULL) & ~0xFFFULL;

    if (first < RAM_START || last > RAM_START + RAM_SIZE)
        return;

    for (uint64_t address = first; address < last; address += MMU_PAGE_SIZE)
    {
        uint64_t n = (address - RAM_START) / MMU_PAGE_SIZE;
        ram_l3[n / 512ULL][n % 512ULL] = page_descriptor_l3(address, flags, DESC_SH_INNER);
    }
}

static int valid_layout()
{
    if (((uint64_t)l1_table & 4095ULL)!= 0)
        return 0;
    if (((uint64_t)exception_vectors_el1 & 2047ULL)!= 0)
        return 0;
    if ((uint64_t)__kernel_exec_start < RAM_START ||
        (uint64_t)__kernel_exec_end > RAM_START + RAM_SIZE ||
        (uint64_t)__kernel_exec_end <= (uint64_t)__kernel_exec_start)
        return 0;
    return 1;
}

static void clean_table_range(const void* start_ptr, uint64_t bytes)
{
    uint64_t start = (uint64_t)start_ptr & ~63ULL;
    uint64_t end = ((uint64_t)start_ptr + bytes + 63ULL) & ~63ULL;
    for (uint64_t p = start; p < end; p += 64ULL)
        asm volatile("dc cvac, %0" :: "r"(p) : "memory");
    asm volatile("dsb sy" ::: "memory");
}

static void clean_page_tables()
{
    clean_table_range(l1_table, sizeof(l1_table));
    clean_table_range(l2_low, sizeof(l2_low));
    clean_table_range(l2_ram, sizeof(l2_ram));
    clean_table_range(ram_l3, sizeof(ram_l3));
}

static void tlb_flush_all()
{
    asm volatile(
        "dsb sy\n"
        "tlbi vmalle1\n"
        "dsb sy\n"
        "isb\n"
        ::: "memory"
    );
}

static void enable_mmu()
{
    uint64_t mair = MAIR_NORMAL_WBWA | (MAIR_DEVICE_nGnRE << 8);
    uint64_t tcr = TCR_T0SZ_39BIT | TCR_IRGN0_WBWA | TCR_ORGN0_WBWA |
                   TCR_SH0_INNER | TCR_EPD1 | TCR_IPS_40BIT;
    uint64_t ttbr0 = (uint64_t)l1_table;

    asm volatile("msr mair_el1, %0" :: "r"(mair) : "memory");
    asm volatile("msr tcr_el1, %0" :: "r"(tcr) : "memory");
    asm volatile("msr ttbr0_el1, %0" :: "r"(ttbr0) : "memory");
    asm volatile("isb" ::: "memory");
    clean_page_tables();
    tlb_flush_all();
    asm volatile("ic iallu\ndsb sy\nisb" ::: "memory");

    uint64_t sctlr = read_sctlr();
    sctlr |= SCTLR_M | SCTLR_C | SCTLR_I;
    asm volatile("msr sctlr_el1, %0\ndsb sy\nisb" :: "r"(sctlr) : "memory");
}

void mmu_init()
{
    uart_puts("MMU: building page tables...\r\n");
    zero_tables();
    map_devices();
    map_ram();
    set_ram_flags((uint64_t)__kernel_exec_start, (uint64_t)__kernel_exec_end, kernel_rx());

    if (!valid_layout())
    {
        uart_puts("MMU: layout validation FAILED.\r\n");
        while (1) asm volatile("wfe");
    }

    uart_puts("MMU: layout validation OK.\r\n");
    uart_puts("MMU: enabling translation...\r\n");
    enable_mmu();
    if (mmu_enabled())
        uart_puts("MMU: enabled.\r\n");
    else
        uart_puts("MMU: enable failed; continuing in identity mode.\r\n");
}

int mmu_enabled() { return (read_sctlr() & SCTLR_M)!= 0; }
uint64_t mmu_read_sctlr() { return read_sctlr(); }
uint64_t mmu_read_ttbr0() { return read_ttbr0(); }
uint64_t mmu_read_tcr() { return read_tcr(); }

static int user_range_ok(uint64_t start, uint64_t end)
{
    return start >= MMU_USER_BASE && end > start && end <= MMU_USER_LIMIT &&
           (start & (MMU_PAGE_SIZE - 1ULL)) == 0;
}

int mmu_user_prepare()
{
    for (uint64_t i = 0; i < MAX_USER_PAGES; ++i)
    {
        user_page_modes[i] = 0;
        uint64_t va = MMU_USER_BASE + i * MMU_PAGE_SIZE;
        uint64_t n = (va - RAM_START) / MMU_PAGE_SIZE;
        ram_l3[n / 512ULL][n % 512ULL] = page_descriptor_l3(va, kernel_rw_nx(), DESC_SH_INNER);
    }

    for (uint64_t va = MMU_USER_STACK_BASE; va < MMU_USER_STACK_TOP; va += MMU_PAGE_SIZE)
    {
        uint64_t n = (va - RAM_START) / MMU_PAGE_SIZE;
        ram_l3[n / 512ULL][n % 512ULL] = page_descriptor_l3(va, kernel_rw_nx(), DESC_SH_INNER);
    }

    user_active = 0;
    tlb_flush_all();
    return 0;
}

int mmu_user_set_range(uint64_t start, uint64_t end, uint32_t flags)
{
    if (end <= start || (start & (MMU_PAGE_SIZE - 1ULL))!= 0)
        return -1;
    if (end > MMU_USER_LIMIT || start < MMU_USER_BASE)
        return -1;
    if ((flags & USER_W)!= 0 && (flags & USER_X)!= 0)
        return -1;

    uint64_t first = start;
    uint64_t last = (end + MMU_PAGE_SIZE - 1ULL) & ~(MMU_PAGE_SIZE - 1ULL);
    if (!user_range_ok(first, last))
        return -1;

    for (uint64_t va = first; va < last; va += MMU_PAGE_SIZE)
    {
        uint64_t page = (va - MMU_USER_BASE) / MMU_PAGE_SIZE;
        user_page_modes[page] = (uint8_t)flags;
        uint64_t ram_page = (va - RAM_START) / MMU_PAGE_SIZE;
        ram_l3[ram_page / 512ULL][ram_page % 512ULL] =
            page_descriptor_l3(va, user_flags(flags), DESC_SH_INNER);
    }

    tlb_flush_all();
    return 0;
}

int mmu_user_finish()
{
    for (uint64_t va = MMU_USER_STACK_BASE; va < MMU_USER_STACK_TOP; va += MMU_PAGE_SIZE)
    {
        uint64_t n = (va - RAM_START) / MMU_PAGE_SIZE;
        uint64_t page = n;
        (void)page;
        ram_l3[n / 512ULL][n % 512ULL] =
            page_descriptor_l3(va, user_flags(USER_R | USER_W), DESC_SH_INNER);
    }
    tlb_flush_all();
    user_active = 1;
    return 0;
}

int mmu_user_pointer_ok(uint64_t address, uint64_t length, int write)
{
    if (!user_active || address == 0 || length == 0)
        return 0;
    if (address < MMU_USER_BASE || address >= MMU_USER_STACK_TOP)
        return 0;
    if (length > MMU_USER_STACK_TOP - address)
        return 0;

    uint64_t end = address + length;
    uint64_t pos = address & ~(MMU_PAGE_SIZE - 1ULL);
    while (pos < end)
    {
        int okay = 0;
        if (pos >= MMU_USER_BASE && pos < MMU_USER_LIMIT)
        {
            uint64_t idx = (pos - MMU_USER_BASE) / MMU_PAGE_SIZE;
            uint8_t mode = user_page_modes[idx];
            okay = (mode & USER_R)!= 0 && (!write || (mode & USER_W)!= 0);
        }
        else if (pos >= MMU_USER_STACK_BASE && pos < MMU_USER_STACK_TOP)
        {
            okay = 1; /* private EL0 stack is always RW */
        }
        if (!okay)
            return 0;
        pos += MMU_PAGE_SIZE;
    }
    return 1;
}

uint64_t mmu_user_stack_top() { return MMU_USER_STACK_TOP; }

void mmu_status()
{
    uart_puts("MMU status\r\n----------\r\n");
    uart_puts("Enabled : "); uart_puts(mmu_enabled()? "yes\r\n" : "no\r\n");
    uart_puts("TTBR0 : 0x");
    uint64_t v = read_ttbr0();
    static const char h[] = "0123456789ABCDEF";
    for (int i = 15; i >= 0; --i) uart_putc(h[(v >> (i * 4)) & 0xF]);
    uart_puts("\r\n");
    uart_puts("User VA : 0x47000000 - 0x47800000\r\n");
    uart_puts("Stack : 0x47F00000 - 0x48000000\r\n");
}