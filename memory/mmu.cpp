#include "mmu.h"
#include "uart.h"
#include "page_alloc.h"

#define RAM_START 0x40000000ULL
#define RAM_SIZE (128ULL * 1024ULL * 1024ULL)
#define L3_ENTRIES 512
#define RAM_L3_TABLES 64
#define MAX_USER_PAGES ((MMU_USER_LIMIT - MMU_USER_BASE) / MMU_PAGE_SIZE)
#define USER_AS_MAX 32
#define USER_L2_FIRST 56
#define USER_L2_COUNT 4
#define STACK_L2_INDEX 63
#define USER_STACK_PAGES ((MMU_USER_STACK_TOP - MMU_USER_STACK_BASE) / MMU_PAGE_SIZE)
#define USER_STACK_FIRST_L3 ((MMU_USER_STACK_BASE - (RAM_START + (uint64_t)STACK_L2_INDEX * 0x200000ULL)) / MMU_PAGE_SIZE)

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

/* P5 address spaces. The L1, RAM L2 and five private L3 tables per process
   are static so that page-table memory is never itself reclaimed while live.
   All unmodified RAM/device tables remain shared read-only by convention. */
struct UserAddressSpace
{
    int used;
    int pid;
    int ready;
};
static UserAddressSpace user_spaces[USER_AS_MAX];
static uint64_t user_l1[USER_AS_MAX][512] __attribute__((aligned(4096)));
static uint64_t user_l2_ram[USER_AS_MAX][512] __attribute__((aligned(4096)));
static uint64_t user_l3_image[USER_AS_MAX][USER_L2_COUNT][512] __attribute__((aligned(4096)));
static uint64_t user_l3_stack[USER_AS_MAX][512] __attribute__((aligned(4096)));
static uint64_t user_frames[USER_AS_MAX][MAX_USER_PAGES];
static uint64_t stack_frames[USER_AS_MAX][USER_STACK_PAGES];
static uint8_t user_space_modes[USER_AS_MAX][MAX_USER_PAGES];
static uint8_t user_stack_modes[USER_AS_MAX][USER_STACK_PAGES];
static int active_user_space = -1;

static void clean_table_range(const void* start_ptr, uint64_t bytes);

static int user_space_find(int pid)
{
    if (pid < 0) return -1;
    for (int i = 0; i < USER_AS_MAX; ++i)
        if (user_spaces[i].used && user_spaces[i].pid == pid)
            return i;
    return -1;
}

static void copy_table(uint64_t* destination, const uint64_t* source)
{
    for (int i = 0; i < 512; ++i) destination[i] = source[i];
}

static void zero_table(uint64_t* table)
{
    for (int i = 0; i < 512; ++i) table[i] = 0;
}

static void user_space_clean_tables(int slot)
{
    clean_table_range(user_l1[slot], sizeof(user_l1[slot]));
    clean_table_range(user_l2_ram[slot], sizeof(user_l2_ram[slot]));
    clean_table_range(user_l3_image[slot], sizeof(user_l3_image[slot]));
    clean_table_range(user_l3_stack[slot], sizeof(user_l3_stack[slot]));
}

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
           (start & (MMU_PAGE_SIZE - 1ULL)) == 0 &&
           (end & (MMU_PAGE_SIZE - 1ULL)) == 0;
}

int mmu_user_prepare()
{
    /* Return to the baseline kernel address space before touching its tables. */
    if (active_user_space >= 0) (void)mmu_user_space_deactivate();
    /* Fail closed before changing descriptors, then invalidate stale TLB state. */
    user_active = 0;
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

    tlb_flush_all();
    return 0;
}

int mmu_user_set_range(uint64_t start, uint64_t end, uint32_t flags)
{
    if (!mmu_enabled() || end <= start ||
        (start & (MMU_PAGE_SIZE - 1ULL)) != 0)
        return -1;
    if (end > MMU_USER_LIMIT || start < MMU_USER_BASE)
        return -1;
    if (flags == 0 || (flags & ~(USER_R | USER_W | USER_X)) != 0 ||
        ((flags & USER_W) != 0 && (flags & USER_X) != 0))
        return -1;

    uint64_t first = start;
    /* end is bounded above, so rounding cannot overflow uint64_t. */
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
    if (!mmu_enabled())
        return -1;

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
    if (address == 0 || length == 0) return 0;
    if (address < MMU_USER_BASE || address >= MMU_USER_STACK_TOP) return 0;
    if (length > MMU_USER_STACK_TOP - address) return 0;
    uint64_t end = address + length;

    if (active_user_space >= 0)
    {
        int slot = active_user_space;
        uint64_t pos = address & ~(MMU_PAGE_SIZE - 1ULL);
        while (pos < end)
        {
            int okay = 0;
            if (pos >= MMU_USER_BASE && pos < MMU_USER_LIMIT)
            {
                uint64_t idx = (pos - MMU_USER_BASE) / MMU_PAGE_SIZE;
                uint8_t mode = user_space_modes[slot][idx];
                okay = user_frames[slot][idx] != 0 && (mode & USER_R) != 0 &&
                       (!write || (mode & USER_W) != 0);
            }
            else if (pos >= MMU_USER_STACK_BASE && pos < MMU_USER_STACK_TOP)
            {
                uint64_t idx = (pos - MMU_USER_STACK_BASE) / MMU_PAGE_SIZE;
                okay = idx < USER_STACK_PAGES && stack_frames[slot][idx] != 0 &&
                       (!write || (user_stack_modes[slot][idx] & USER_W) != 0);
            }
            if (!okay) return 0;
            pos += MMU_PAGE_SIZE;
        }
        return 1;
    }

    if (!user_active) return 0;
    uint64_t pos = address & ~(MMU_PAGE_SIZE - 1ULL);
    while (pos < end)
    {
        int okay = 0;
        if (pos >= MMU_USER_BASE && pos < MMU_USER_LIMIT)
        {
            uint64_t idx = (pos - MMU_USER_BASE) / MMU_PAGE_SIZE;
            uint8_t mode = user_page_modes[idx];
            okay = (mode & USER_R) != 0 && (!write || (mode & USER_W) != 0);
        }
        else if (pos >= MMU_USER_STACK_BASE && pos < MMU_USER_STACK_TOP)
        {
            okay = 1; /* compatibility path: legacy single-image stack */
        }
        if (!okay) return 0;
        pos += MMU_PAGE_SIZE;
    }
    return 1;
}

uint64_t mmu_user_stack_top() { return MMU_USER_STACK_TOP; }


int mmu_user_space_exists(int pid)
{
    return user_space_find(pid) >= 0;
}

int mmu_user_space_active_pid()
{
    return active_user_space >= 0 ? user_spaces[active_user_space].pid : -1;
}

int mmu_user_space_create(int pid)
{
    if (!mmu_enabled() || pid < 0 || !page_alloc_ready()) return -1;
    if (user_space_find(pid) >= 0) return -2;

    int slot = -1;
    for (int i = 0; i < USER_AS_MAX; ++i)
        if (!user_spaces[i].used) { slot = i; break; }
    if (slot < 0) return -3;

    user_spaces[slot].used = 1;
    user_spaces[slot].pid = pid;
    user_spaces[slot].ready = 0;
    for (uint64_t i = 0; i < MAX_USER_PAGES; ++i)
    {
        user_frames[slot][i] = 0;
        user_space_modes[slot][i] = 0;
    }
    for (uint64_t i = 0; i < USER_STACK_PAGES; ++i)
    {
        stack_frames[slot][i] = 0;
        user_stack_modes[slot][i] = 0;
    }

    copy_table(user_l1[slot], l1_table);
    copy_table(user_l2_ram[slot], l2_ram);
    user_l1[slot][0] = table_descriptor((uint64_t)l2_low);
    user_l1[slot][1] = table_descriptor((uint64_t)user_l2_ram[slot]);

    for (int t = 0; t < USER_L2_COUNT; ++t)
    {
        copy_table(user_l3_image[slot][t], ram_l3[USER_L2_FIRST + t]);
        zero_table(user_l3_image[slot][t]);
        user_l2_ram[slot][USER_L2_FIRST + t] =
            table_descriptor((uint64_t)user_l3_image[slot][t]);
    }

    copy_table(user_l3_stack[slot], ram_l3[STACK_L2_INDEX]);
    for (int i = 256; i < 512; ++i) user_l3_stack[slot][i] = 0;
    user_l2_ram[slot][STACK_L2_INDEX] = table_descriptor((uint64_t)user_l3_stack[slot]);

    /* Only the top 64 KiB is mapped as EL0 stack. Each page has an independent
       physical backing page; the lower part of the previous 1 MiB range is a
       guard region, so underflow becomes a permission/translation fault. */
    for (uint64_t i = 0; i < USER_STACK_PAGES; ++i)
    {
        void* frame = allocate_page();
        if (frame == 0)
        {
            (void)mmu_user_space_destroy(pid);
            return -4;
        }
        stack_frames[slot][i] = (uint64_t)frame;
        user_stack_modes[slot][i] = (uint8_t)(USER_R | USER_W);
        uint64_t l3_index = USER_STACK_FIRST_L3 + i;
        user_l3_stack[slot][l3_index] =
            page_descriptor_l3((uint64_t)frame, user_flags(USER_R | USER_W), DESC_SH_INNER);
    }

    user_space_clean_tables(slot);
    return 0;
}

int mmu_user_space_map(int pid, uint64_t start, uint64_t end, uint32_t flags)
{
    int slot = user_space_find(pid);
    if (slot < 0 || user_spaces[slot].ready || !mmu_enabled() || end <= start ||
        start < MMU_USER_BASE || end > MMU_USER_LIMIT ||
        (start & (MMU_PAGE_SIZE - 1ULL)) != 0 || flags == 0 ||
        (flags & ~(USER_R | USER_W | USER_X)) != 0 ||
        ((flags & USER_W) != 0 && (flags & USER_X) != 0))
        return -1;

    uint64_t rounded_end = (end + MMU_PAGE_SIZE - 1ULL) & ~(MMU_PAGE_SIZE - 1ULL);
    if (rounded_end < end || rounded_end > MMU_USER_LIMIT) return -1;
    uint64_t first = (start - MMU_USER_BASE) / MMU_PAGE_SIZE;
    uint64_t last = (rounded_end - MMU_USER_BASE) / MMU_PAGE_SIZE;
    if (last <= first || last > MAX_USER_PAGES) return -1;

    for (uint64_t i = first; i < last; ++i)
        if (user_frames[slot][i] != 0 || user_space_modes[slot][i] != 0)
            return -2;

    uint64_t i = first;
    for (; i < last; ++i)
    {
        void* frame = allocate_page();
        if (frame == 0) break;
        user_frames[slot][i] = (uint64_t)frame;
        user_space_modes[slot][i] = (uint8_t)flags;
        uint64_t va = MMU_USER_BASE + i * MMU_PAGE_SIZE;
        uint64_t table_index = (va - RAM_START) / 0x200000ULL;
        uint64_t l3_index = (va >> 12) & 0x1FFULL;
        if (table_index < USER_L2_FIRST || table_index >= USER_L2_FIRST + USER_L2_COUNT)
        {
            (void)mmu_user_space_destroy(pid);
            return -3;
        }
        user_l3_image[slot][table_index - USER_L2_FIRST][l3_index] =
            page_descriptor_l3((uint64_t)frame, user_flags(flags), DESC_SH_INNER);
    }
    if (i != last)
    {
        for (uint64_t j = first; j < i; ++j)
        {
            (void)free_page((void*)user_frames[slot][j]);
            user_frames[slot][j] = 0;
            user_space_modes[slot][j] = 0;
            uint64_t va = MMU_USER_BASE + j * MMU_PAGE_SIZE;
            uint64_t table_index = (va - RAM_START) / 0x200000ULL;
            uint64_t l3_index = (va >> 12) & 0x1FFULL;
            user_l3_image[slot][table_index - USER_L2_FIRST][l3_index] = 0;
        }
        return -4;
    }
    return 0;
}

void* mmu_user_space_pointer(int pid, uint64_t address)
{
    int slot = user_space_find(pid);
    if (slot < 0) return 0;
    if (address >= MMU_USER_BASE && address < MMU_USER_LIMIT)
    {
        uint64_t index = (address - MMU_USER_BASE) / MMU_PAGE_SIZE;
        if (user_frames[slot][index] == 0) return 0;
        return (void*)(user_frames[slot][index] + (address & (MMU_PAGE_SIZE - 1ULL)));
    }
    if (address >= MMU_USER_STACK_BASE && address < MMU_USER_STACK_TOP)
    {
        uint64_t index = (address - MMU_USER_STACK_BASE) / MMU_PAGE_SIZE;
        if (index >= USER_STACK_PAGES || stack_frames[slot][index] == 0) return 0;
        return (void*)(stack_frames[slot][index] + (address & (MMU_PAGE_SIZE - 1ULL)));
    }
    return 0;
}

int mmu_user_space_copy_to(int pid, uint64_t destination, const void* source, uint64_t length)
{
    if (source == 0 || length == 0 || destination < MMU_USER_BASE ||
        destination >= MMU_USER_LIMIT || length > MMU_USER_LIMIT - destination)
        return -1;
    const uint8_t* src = (const uint8_t*)source;
    uint64_t copied = 0;
    while (copied < length)
    {
        uint64_t address = destination + copied;
        uint64_t chunk = MMU_PAGE_SIZE - (address & (MMU_PAGE_SIZE - 1ULL));
        if (chunk > length - copied) chunk = length - copied;
        uint8_t* dst = (uint8_t*)mmu_user_space_pointer(pid, address);
        if (dst == 0) return -2;
        for (uint64_t j = 0; j < chunk; ++j) dst[j] = src[copied + j];
        copied += chunk;
    }
    return 0;
}

int mmu_user_space_finish(int pid)
{
    int slot = user_space_find(pid);
    if (slot < 0 || !mmu_enabled()) return -1;
    int mapped = 0;
    for (uint64_t i = 0; i < MAX_USER_PAGES; ++i)
        if (user_frames[slot][i] != 0) mapped = 1;
    if (!mapped) return -2;
    user_spaces[slot].ready = 1;
    user_space_clean_tables(slot);
    asm volatile("dsb sy" ::: "memory");
    return 0;
}

int mmu_user_space_activate(int pid)
{
    int slot = user_space_find(pid);
    if (slot < 0 || !user_spaces[slot].ready || !mmu_enabled()) return -1;
    user_space_clean_tables(slot);
    asm volatile("dsb sy" ::: "memory");
    uint64_t root = (uint64_t)user_l1[slot];
    asm volatile("msr ttbr0_el1, %0\n isb" :: "r"(root) : "memory");
    tlb_flush_all();
    active_user_space = slot;
    user_active = 0;
    return 0;
}

int mmu_user_space_deactivate()
{
    if (active_user_space < 0) return 0;
    uint64_t root = (uint64_t)l1_table;
    asm volatile("msr ttbr0_el1, %0\n isb" :: "r"(root) : "memory");
    tlb_flush_all();
    active_user_space = -1;
    user_active = 0;
    return 0;
}

int mmu_user_space_destroy(int pid)
{
    int slot = user_space_find(pid);
    if (slot < 0) return -1;
    if (active_user_space == slot) (void)mmu_user_space_deactivate();

    for (uint64_t i = 0; i < MAX_USER_PAGES; ++i)
    {
        if (user_frames[slot][i] != 0)
            (void)free_page((void*)user_frames[slot][i]);
        user_frames[slot][i] = 0;
        user_space_modes[slot][i] = 0;
    }
    for (uint64_t i = 0; i < USER_STACK_PAGES; ++i)
    {
        if (stack_frames[slot][i] != 0)
            (void)free_page((void*)stack_frames[slot][i]);
        stack_frames[slot][i] = 0;
        user_stack_modes[slot][i] = 0;
    }
    user_spaces[slot].used = 0;
    user_spaces[slot].pid = -1;
    user_spaces[slot].ready = 0;
    return 0;
}

int mmu_user_space_sync_code(int pid, uint64_t start, uint64_t end)
{
    int slot = user_space_find(pid);
    if (slot < 0 || !user_spaces[slot].ready || start < MMU_USER_BASE ||
        end <= start || end > MMU_USER_LIMIT)
        return -1;
    if (mmu_user_space_activate(pid) != 0) return -2;
    uint64_t begin = start & ~63ULL;
    uint64_t finish = (end + 63ULL) & ~63ULL;
    for (uint64_t p = begin; p < finish; p += 64ULL)
    {
        uint64_t page = (p - MMU_USER_BASE) / MMU_PAGE_SIZE;
        if (page < MAX_USER_PAGES && (user_space_modes[slot][page] & USER_X) != 0)
            asm volatile("dc cvau, %0" :: "r"(p) : "memory");
    }
    asm volatile("dsb ish" ::: "memory");
    for (uint64_t p = begin; p < finish; p += 64ULL)
    {
        uint64_t page = (p - MMU_USER_BASE) / MMU_PAGE_SIZE;
        if (page < MAX_USER_PAGES && (user_space_modes[slot][page] & USER_X) != 0)
            asm volatile("ic ivau, %0" :: "r"(p) : "memory");
    }
    asm volatile("dsb ish\n isb" ::: "memory");
    (void)mmu_user_space_deactivate();
    return 0;
}

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
    uart_puts("Stack : 0x47FF0000 - 0x48000000 (64 KiB; guard below)\r\n");
}