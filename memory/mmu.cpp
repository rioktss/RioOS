/*
 * MIT License
 *
 * Copyright (c) 2026 Rio okta kusuma
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files, to deal in the Software
 * without restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies of the
 * Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "mmu.h"
#include "uart.h"
#include "page_alloc.h"
#include "memory.h"
#include "random.h"
#include "smp.h"

#define RAM_START 0x40000000ULL
#define RAM_SIZE (128ULL * 1024ULL * 1024ULL)
#define L3_ENTRIES 512
#define RAM_L3_TABLES 64

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
#define USER_PERM_MASK 7U
#define PTE_COW (1ULL << 55)          /* software bit: frame shared copy-on-write, hardware read-only until the first write */
#define PTE_ADDR_MASK 0x0000FFFFFFFFF000ULL

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

static void clean_table_range(const void* start_ptr, uint64_t bytes);

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

    /* PCI: BAR window 0x10000000..0x13FFFFFF (assigned by the PCI driver) as 2 MB device blocks,
       and the PCIe ECAM region (0x4010000000, inside the 1 GB block at L1[256]). */
    for (uint64_t address = 0x10000000ULL; address < 0x14000000ULL; address += 0x200000ULL)
        l2_low[(address >> 21) & 0x1FFULL] = page_descriptor(address, device_rw_nx(), DESC_SH_OUTER);
    l1_table[256] = page_descriptor(0x4000000000ULL, device_rw_nx(), DESC_SH_OUTER);
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

/* Kernel stack guard: the page is removed from (on) / put back into (off) the identity map shared by every address space, so a stray
   access faults instead of corrupting the neighbouring heap block. */
void mmu_kernel_guard_page(uint64_t addr, int on)
{
    addr &= ~0xFFFULL;
    if (addr < RAM_START || addr >= RAM_START + RAM_SIZE) return;
    uint64_t n = (addr - RAM_START) / MMU_PAGE_SIZE;
    uint64_t* e = &ram_l3[n / 512ULL][n % 512ULL];
    *e = on ? 0ULL : page_descriptor_l3(addr, kernel_rw_nx(), DESC_SH_INNER);
    asm volatile("dc cvac, %0\n dsb ish\n tlbi vaae1is, %1\n dsb ish\n isb" :: "r"(e), "r"(addr >> 12) : "memory");
}

int mmu_enabled() { return (read_sctlr() & SCTLR_M)!= 0; }
uint64_t mmu_read_sctlr() { return read_sctlr(); }
uint64_t mmu_read_ttbr0() { return read_ttbr0(); }
uint64_t mmu_read_tcr() { return read_tcr(); }

/* =====================================================================================================================
   User address spaces

   Every process has a private translation table tree (its own L1 in TTBR0). The kernel's identity mappings (L1[0] devices,
   L1[1] RAM, L1[256] PCI ECAM) are shared by every tree; the user part is L1[4]..L1[254], i.e. 0x1_0000_0000 .. 0x3F_C000_0000
   (252 GiB), whose L2/L3 tables are allocated on demand from the page pool and freed when the process dies. A sorted array of
   VMAs records what each range is (image, heap, stack, anonymous or shared mapping) and with which permissions; pages are
   faulted in lazily and shared copy-on-write across fork. The stack top, the mmap area and the heap start are randomised per exec.
   ===================================================================================================================== */

#define VMA_ANON   1U      /* lazily zero-filled */
#define VMA_SHARED 2U      /* frames installed explicitly (file / shm mappings), never copy-on-write */
#define VMA_STACK  4U
#define VMA_HEAP   8U
#define VMA_IMAGE  16U     /* ELF segment, populated eagerly */
#define VMA_LAZY   (VMA_ANON | VMA_STACK | VMA_HEAP)

#define USER_STACK_MAX (8ULL * 1024ULL * 1024ULL)
#define USER_STACK_GAP (1ULL << 20)
#define USER_L1_FIRST (MMU_USER_BASE >> 30)
#define USER_L1_LAST  (MMU_USER_LIMIT >> 30)       /* exclusive */

struct Vma
{
    uint64_t start;
    uint64_t end;
    uint32_t prot;
    uint32_t flags;
};

struct UserAddressSpace
{
    int pid;
    int ready;
    uint64_t* l1;
    Vma* vma;
    int nvma;
    int capvma;
    uint64_t brk_start;
    uint64_t brk_cur;
    uint64_t mmap_top;
    uint64_t stack_top;
    uint64_t stack_lo;
    uint64_t image_end;
    uint64_t cow_faults;
    uint64_t demand_faults;
    uint64_t table_pages;
};

static UserAddressSpace** spaces = 0;
static int nspaces = 0;
static int capspaces = 0;
static UserAddressSpace* active_as[SMP_MAX_CPUS];

static uint64_t page_align_up(uint64_t v) { return (v + MMU_PAGE_SIZE - 1ULL) & ~(MMU_PAGE_SIZE - 1ULL); }

static void clean_entry(uint64_t* entry)
{
    asm volatile("dc cvac, %0\n dsb ish" :: "r"(entry) : "memory");
}

static void tlb_flush_all_is()
{
    asm volatile("dsb ishst\n tlbi vmalle1is\n dsb ish\n isb" ::: "memory");
}

static void tlb_flush_va_is(uint64_t va)
{
    asm volatile("dsb ishst\n tlbi vaae1is, %0\n dsb ish\n isb" :: "r"(va >> 12) : "memory");
}

static void tlb_flush_va_local(uint64_t va)
{
    asm volatile("dsb ishst\n tlbi vaae1, %0\n dsb nsh\n isb" :: "r"(va >> 12) : "memory");
}

/* A user translation can only be cached by a CPU that has the address space active (every activation flushes the whole TLB), so
   the expensive inner-shareable broadcast is needed only while another CPU is running the same address space. */
static int as_active_elsewhere(const UserAddressSpace* as)
{
    int me = (int)smp_cpu_id_fast();
    for (int c = 0; c < SMP_MAX_CPUS; ++c) if (c != me && active_as[c] == as) return 1;
    return 0;
}
static void tlb_flush_as(const UserAddressSpace* as) { if (as_active_elsewhere(as)) tlb_flush_all_is(); else tlb_flush_all(); }
static void tlb_flush_as_va(const UserAddressSpace* as, uint64_t va) { if (as_active_elsewhere(as)) tlb_flush_va_is(va); else tlb_flush_va_local(va); }

static void copy_frame(uint64_t dst, uint64_t src)
{
    volatile uint64_t* d = (volatile uint64_t*)dst;
    const volatile uint64_t* s2 = (const volatile uint64_t*)src;
    for (uint64_t i = 0; i < MMU_PAGE_SIZE / 8ULL; ++i) d[i] = s2[i];
}

static uint64_t rand_u64()
{
    uint64_t r = 0;
    random_get((uint8_t*)&r, 8);
    return r;
}

static UserAddressSpace* space_find(int pid)
{
    if (pid < 0) return 0;
    for (int i = 0; i < nspaces; ++i)
        if (spaces[i]->pid == pid) return spaces[i];
    return 0;
}

static int space_register(UserAddressSpace* as)
{
    if (nspaces == capspaces)
    {
        int ncap = capspaces ? capspaces * 2 : 32;
        UserAddressSpace** n = (UserAddressSpace**)kmalloc((uint64_t)ncap * sizeof(UserAddressSpace*));
        if (!n) return -1;
        for (int i = 0; i < nspaces; ++i) n[i] = spaces[i];
        if (spaces) kfree(spaces);
        spaces = n;
        capspaces = ncap;
    }
    spaces[nspaces++] = as;
    return 0;
}

static void space_unregister(UserAddressSpace* as)
{
    for (int i = 0; i < nspaces; ++i)
        if (spaces[i] == as) { spaces[i] = spaces[--nspaces]; return; }
}

/* ---- page table entries ---- */

static uint64_t make_pte(uint64_t frame, uint32_t perm, int cow)
{
    uint32_t p = perm & USER_PERM_MASK;
    if (cow) p &= ~USER_W;
    uint64_t flags = p ? user_flags(p) : (DESC_ATTR_NORMAL | DESC_AP_EL1_RW | DESC_PXN | DESC_UXN);   /* PROT_NONE: kernel-only */
    uint64_t pte = page_descriptor_l3(frame, flags, DESC_SH_INNER);
    if (cow) pte |= PTE_COW;
    return pte;
}

static uint64_t* new_table(UserAddressSpace* as)
{
    uint64_t* t = (uint64_t*)allocate_page();
    if (t) ++as->table_pages;
    return t;
}

/* The L3 slot of `va`; builds the missing L2/L3 tables when `create`. 0 when absent (or out of memory). */
static uint64_t* pte_walk(UserAddressSpace* as, uint64_t va, int create)
{
    if (va < MMU_USER_BASE || va >= MMU_USER_LIMIT) return 0;
    uint64_t* t = as->l1;
    for (int level = 0; level < 2; ++level)
    {
        uint64_t idx = level == 0 ? ((va >> 30) & 0x1FFULL) : ((va >> 21) & 0x1FFULL);
        uint64_t d = t[idx];
        if (!(d & DESC_VALID))
        {
            if (!create) return 0;
            uint64_t* n = new_table(as);
            if (!n) return 0;
            t[idx] = table_descriptor((uint64_t)n);
            clean_entry(&t[idx]);
            t = n;
        }
        else t = (uint64_t*)(d & PTE_ADDR_MASK);
    }
    return &t[(va >> 12) & 0x1FFULL];
}

/* Next present page at or after *va (below `end`), skipping absent tables in one step. */
static uint64_t* pte_next(UserAddressSpace* as, uint64_t* va_io, uint64_t end)
{
    uint64_t va = *va_io;
    while (va < end)
    {
        uint64_t d1 = as->l1[(va >> 30) & 0x1FFULL];
        if (!(d1 & DESC_VALID)) { va = (va & ~((1ULL << 30) - 1ULL)) + (1ULL << 30); continue; }
        uint64_t* l2 = (uint64_t*)(d1 & PTE_ADDR_MASK);
        uint64_t d2 = l2[(va >> 21) & 0x1FFULL];
        if (!(d2 & DESC_VALID)) { va = (va & ~((1ULL << 21) - 1ULL)) + (1ULL << 21); continue; }
        uint64_t* l3 = (uint64_t*)(d2 & PTE_ADDR_MASK);
        uint64_t* e = &l3[(va >> 12) & 0x1FFULL];
        if (*e & DESC_VALID) { *va_io = va; return e; }
        va += MMU_PAGE_SIZE;
    }
    *va_io = end;
    return 0;
}

/* ---- VMAs ---- */

static int vma_lower_bound(const UserAddressSpace* as, uint64_t addr)   /* first VMA that ends above addr */
{
    int lo = 0, hi = as->nvma;
    while (lo < hi)
    {
        int mid = (lo + hi) / 2;
        if (as->vma[mid].end > addr) hi = mid; else lo = mid + 1;
    }
    return lo;
}

static Vma* vma_find(const UserAddressSpace* as, uint64_t va)
{
    int i = vma_lower_bound(as, va);
    if (i < as->nvma && as->vma[i].start <= va) return &as->vma[i];
    return 0;
}

static int vma_grow(UserAddressSpace* as)
{
    if (as->nvma < as->capvma) return 0;
    int ncap = as->capvma ? as->capvma * 2 : 16;
    Vma* n = (Vma*)kmalloc((uint64_t)ncap * sizeof(Vma));
    if (!n) return -1;
    for (int i = 0; i < as->nvma; ++i) n[i] = as->vma[i];
    if (as->vma) kfree(as->vma);
    as->vma = n;
    as->capvma = ncap;
    return 0;
}

static int vma_insert(UserAddressSpace* as, int idx, uint64_t start, uint64_t end, uint32_t prot, uint32_t flags)
{
    if (vma_grow(as) != 0) return -1;
    for (int i = as->nvma; i > idx; --i) as->vma[i] = as->vma[i - 1];
    as->vma[idx].start = start;
    as->vma[idx].end = end;
    as->vma[idx].prot = prot;
    as->vma[idx].flags = flags;
    ++as->nvma;
    return 0;
}

static void vma_erase(UserAddressSpace* as, int idx)
{
    for (int i = idx; i + 1 < as->nvma; ++i) as->vma[i] = as->vma[i + 1];
    --as->nvma;
}

static int vma_range_free(const UserAddressSpace* as, uint64_t a, uint64_t b)
{
    int i = vma_lower_bound(as, a);
    return !(i < as->nvma && as->vma[i].start < b);
}

static int vma_range_fully_mapped(const UserAddressSpace* as, uint64_t a, uint64_t b)
{
    uint64_t pos = a;
    int i = vma_lower_bound(as, a);
    while (pos < b)
    {
        if (i >= as->nvma || as->vma[i].start > pos) return 0;
        pos = as->vma[i].end;
        ++i;
    }
    return 1;
}

/* Make `addr` a VMA boundary. */
static int vma_split(UserAddressSpace* as, uint64_t addr)
{
    int i = vma_lower_bound(as, addr);
    if (i >= as->nvma) return 0;
    Vma v = as->vma[i];
    if (v.start >= addr || v.end <= addr) return 0;
    if (vma_grow(as) != 0) return -1;
    as->vma[i].end = addr;
    return vma_insert(as, i + 1, addr, v.end, v.prot, v.flags);
}

/* Merge anonymous neighbours with identical attributes so repeated mmap/munmap does not fragment the array. */
static void vma_try_merge(UserAddressSpace* as, int idx)
{
    if (idx + 1 < as->nvma)
    {
        Vma* a = &as->vma[idx]; Vma* b = &as->vma[idx + 1];
        if (a->end == b->start && a->prot == b->prot && a->flags == b->flags && a->flags == VMA_ANON) { a->end = b->end; vma_erase(as, idx + 1); }
    }
    if (idx > 0)
    {
        Vma* a = &as->vma[idx - 1]; Vma* b = &as->vma[idx];
        if (a->end == b->start && a->prot == b->prot && a->flags == b->flags && a->flags == VMA_ANON) { a->end = b->end; vma_erase(as, idx); }
    }
}

/* Highest free gap of `len` bytes below mmap_top and above the heap start. 0 when there is none. */
static uint64_t vma_find_gap_topdown(const UserAddressSpace* as, uint64_t len)
{
    uint64_t lo = as->brk_start ? as->brk_start : (as->image_end ? as->image_end : MMU_USER_BASE);
    uint64_t top = as->mmap_top;
    for (int i = vma_lower_bound(as, top) - 1; i >= -1; --i)
    {
        uint64_t floor = lo;
        if (i >= 0)
        {
            const Vma* v = &as->vma[i];
            if (v->end > top) { if (v->start < top) top = v->start; continue; }
            floor = v->end > lo ? v->end : lo;
        }
        if (top > floor && top - floor >= len) return top - len;
        if (i >= 0) { if (as->vma[i].start < top) top = as->vma[i].start; }
        if (top <= lo) return 0;
    }
    return 0;
}

/* Remove the pages of [a, b): clears the entries and drops one reference on every frame. The caller flushes the TLBs. */
static void unmap_pages(UserAddressSpace* as, uint64_t a, uint64_t b)
{
    uint64_t va = a;
    uint64_t* e;
    while ((e = pte_next(as, &va, b)) != 0)
    {
        uint64_t frame = *e & PTE_ADDR_MASK;
        *e = 0;
        clean_entry(e);
        tlb_flush_as_va(as, va);
        (void)free_page((void*)frame);
        va += MMU_PAGE_SIZE;
    }
}

/* Resolve a lazy or copy-on-write page so an access of the requested kind succeeds. 1 = usable afterwards. */
static int user_page_ensure(UserAddressSpace* as, uint64_t va, int write, int exec)
{
    va &= ~(MMU_PAGE_SIZE - 1ULL);
    if (va < MMU_USER_BASE || va >= MMU_USER_LIMIT) return 0;
    Vma* v = vma_find(as, va);
    if (!v || (v->prot & USER_PERM_MASK) == 0) return 0;
    if (write && !(v->prot & USER_W)) return 0;
    if (exec && !(v->prot & USER_X)) return 0;
    uint64_t* e = pte_walk(as, va, 1);
    if (!e) return 0;
    uint64_t pte = *e;

    if (!(pte & DESC_VALID))
    {
        if (!(v->flags & VMA_LAZY)) return 0;
        void* fresh = allocate_page();
        if (!fresh) return 0;
        *e = make_pte((uint64_t)fresh, v->prot, 0);
        clean_entry(e);
        ++as->demand_faults;
        return 1;
    }

    uint64_t frame = pte & PTE_ADDR_MASK;
    if (write && (pte & PTE_COW))
    {
        uint64_t nf = frame;
        if (page_refcount((void*)frame) > 1)
        {
            void* copy = allocate_page();
            if (!copy) return 0;
            copy_frame((uint64_t)copy, frame);
            nf = (uint64_t)copy;
        }
        *e = 0;                                  /* break before make: the output address changes */
        clean_entry(e);
        tlb_flush_as_va(as, va);
        if (nf != frame) (void)free_page((void*)frame);
        *e = make_pte(nf, v->prot, 0);
        clean_entry(e);
        ++as->cow_faults;
        return 1;
    }

    /* Present and not copy-on-write: bring the entry in line with the VMA (it may lag behind after an mprotect race). */
    uint64_t want = make_pte(frame, v->prot, (pte & PTE_COW) != 0);
    if (want != pte)
    {
        *e = want;
        clean_entry(e);
        tlb_flush_as_va(as, va);
    }
    return 1;
}

int mmu_user_fault_ex(uint64_t far, int is_write, int is_exec)
{
    UserAddressSpace* as = active_as[smp_cpu_id_fast()];
    if (!as) return 0;
    int ok = user_page_ensure(as, far, is_write, is_exec);
    if (ok) tlb_flush_va_local(far & ~(MMU_PAGE_SIZE - 1ULL));   /* a stale read-only translation must not fault again */
    return ok;
}

int mmu_user_fault(uint64_t far, int is_write)
{
    return mmu_user_fault_ex(far, is_write, 0);
}

int mmu_user_pointer_ok(uint64_t address, uint64_t length, int write)
{
    if (address == 0 || length == 0) return 0;
    if (address < MMU_USER_BASE || address >= MMU_USER_LIMIT) return 0;
    if (length > MMU_USER_LIMIT - address) return 0;
    UserAddressSpace* as = active_as[smp_cpu_id_fast()];
    if (!as) return 0;
    uint64_t end = address + length;
    for (uint64_t pos = address & ~(MMU_PAGE_SIZE - 1ULL); pos < end; pos += MMU_PAGE_SIZE)
        if (!user_page_ensure(as, pos, write, 0)) return 0;
    return 1;
}

/* ---- lifetime ---- */

static void space_free(UserAddressSpace* as)
{
    if (as->l1)
    {
        for (uint64_t i1 = USER_L1_FIRST; i1 < USER_L1_LAST; ++i1)
        {
            uint64_t d1 = as->l1[i1];
            if (!(d1 & DESC_VALID)) continue;
            uint64_t* l2 = (uint64_t*)(d1 & PTE_ADDR_MASK);
            for (int i2 = 0; i2 < 512; ++i2)
            {
                uint64_t d2 = l2[i2];
                if (!(d2 & DESC_VALID)) continue;
                uint64_t* l3 = (uint64_t*)(d2 & PTE_ADDR_MASK);
                for (int i3 = 0; i3 < 512; ++i3)
                    if (l3[i3] & DESC_VALID) (void)free_page((void*)(l3[i3] & PTE_ADDR_MASK));
                (void)free_page(l3);
            }
            (void)free_page(l2);
        }
        (void)free_page(as->l1);
    }
    if (as->vma) kfree(as->vma);
    kfree(as);
}

static UserAddressSpace* space_alloc(int pid)
{
    UserAddressSpace* as = (UserAddressSpace*)kmalloc(sizeof(UserAddressSpace));
    if (!as) return 0;
    uint8_t* b = (uint8_t*)as;
    for (uint64_t i = 0; i < sizeof(UserAddressSpace); ++i) b[i] = 0;
    as->pid = pid;
    as->l1 = (uint64_t*)allocate_page();
    if (!as->l1) { kfree(as); return 0; }
    as->table_pages = 1;
    as->l1[0] = l1_table[0];
    as->l1[1] = l1_table[1];
    as->l1[256] = l1_table[256];
    clean_table_range(as->l1, MMU_PAGE_SIZE);
    return as;
}

int mmu_user_space_create(int pid)
{
    if (!mmu_enabled() || pid < 0 || !page_alloc_ready()) return -1;
    if (space_find(pid)) return -2;
    UserAddressSpace* as = space_alloc(pid);
    if (!as) return -3;

    uint64_t r = rand_u64();
    as->stack_top = MMU_USER_LIMIT - ((r & 0xFFFFULL) << 12);                       /* up to 256 MiB below the limit */
    as->stack_lo = as->stack_top - USER_STACK_MAX;
    as->mmap_top = as->stack_lo - USER_STACK_GAP - (((r >> 16) & 0x3FFFFULL) << 12);   /* up to 1 GiB further down */

    if (vma_insert(as, 0, as->stack_lo, as->stack_top, USER_R | USER_W, VMA_STACK) != 0) { space_free(as); return -4; }
    /* The top pages are backed up front; the rest of the stack is demand-paged. */
    for (uint64_t i = 1; i <= 2; ++i)
    {
        uint64_t va = as->stack_top - i * MMU_PAGE_SIZE;
        uint64_t* e = pte_walk(as, va, 1);
        void* f = e ? allocate_page() : 0;
        if (!f) { space_free(as); return -4; }
        *e = make_pte((uint64_t)f, USER_R | USER_W, 0);
        clean_entry(e);
    }
    if (space_register(as) != 0) { space_free(as); return -4; }
    return 0;
}

int mmu_user_space_map(int pid, uint64_t start, uint64_t end, uint32_t flags)
{
    UserAddressSpace* as = space_find(pid);
    if (!as || as->ready || !mmu_enabled() || end <= start ||
        start < MMU_USER_BASE || end > MMU_USER_LIMIT ||
        (start & (MMU_PAGE_SIZE - 1ULL)) != 0 || flags == 0 ||
        (flags & ~(USER_R | USER_W | USER_X)) != 0 ||
        ((flags & USER_W) != 0 && (flags & USER_X) != 0))
        return -1;

    uint64_t rounded_end = page_align_up(end);
    if (rounded_end < end || rounded_end > MMU_USER_LIMIT) return -1;
    if (!vma_range_free(as, start, rounded_end)) return -2;
    int idx = vma_lower_bound(as, start);
    if (vma_insert(as, idx, start, rounded_end, flags, VMA_IMAGE) != 0) return -4;

    uint64_t va = start;
    for (; va < rounded_end; va += MMU_PAGE_SIZE)
    {
        uint64_t* e = pte_walk(as, va, 1);
        void* f = e ? allocate_page() : 0;
        if (!f) break;
        *e = make_pte((uint64_t)f, flags, 0);
        clean_entry(e);
    }
    if (va != rounded_end)
    {
        unmap_pages(as, start, va);
        tlb_flush_as(as);
        vma_erase(as, vma_lower_bound(as, start));
        return -4;
    }
    if (rounded_end > as->image_end) as->image_end = rounded_end;
    return 0;
}

void* mmu_user_space_pointer(int pid, uint64_t address)
{
    UserAddressSpace* as = space_find(pid);
    if (!as) return 0;
    uint64_t* e = pte_walk(as, address, 0);
    if (!e || !(*e & DESC_VALID)) return 0;
    return (void*)((*e & PTE_ADDR_MASK) + (address & (MMU_PAGE_SIZE - 1ULL)));
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
    UserAddressSpace* as = space_find(pid);
    if (!as || !mmu_enabled()) return -1;
    if (as->image_end == 0) return -2;
    uint64_t gap = (rand_u64() & 0x1FFFULL) << 12;                  /* up to 32 MiB of heap randomisation */
    as->brk_start = as->brk_cur = as->image_end + MMU_PAGE_SIZE + gap;    /* one guard page above the image */
    as->ready = 1;
    asm volatile("dsb sy" ::: "memory");
    return 0;
}

uint64_t mmu_user_space_stack_top(int pid)
{
    UserAddressSpace* as = space_find(pid);
    return as ? as->stack_top : 0;
}

uint64_t mmu_user_stack_top()
{
    UserAddressSpace* as = active_as[smp_cpu_id_fast()];
    return as ? as->stack_top : 0;
}

int mmu_user_space_exists(int pid) { return space_find(pid) != 0; }

int mmu_user_space_active_pid()
{
    UserAddressSpace* as = active_as[smp_cpu_id_fast()];
    return as ? as->pid : -1;
}

int mmu_user_space_is_loaded(int pid)
{
    UserAddressSpace* as = active_as[smp_cpu_id_fast()];
    if (!as || as->pid != pid) return 0;
    return read_ttbr0() == (uint64_t)as->l1;
}

void mmu_ensure_kernel_space()
{
    if (!mmu_enabled()) return;
    if (read_ttbr0() != (uint64_t)l1_table)
    {
        asm volatile("msr ttbr0_el1, %0\n isb" :: "r"((uint64_t)l1_table) : "memory");
        tlb_flush_all();
    }
    active_as[smp_cpu_id_fast()] = 0;
}

int mmu_user_space_activate(int pid)
{
    UserAddressSpace* as = space_find(pid);
    if (!as || !as->ready || !mmu_enabled()) return -1;
    asm volatile("dsb sy" ::: "memory");
    asm volatile("msr ttbr0_el1, %0\n isb" :: "r"((uint64_t)as->l1) : "memory");
    tlb_flush_all();
    active_as[smp_cpu_id_fast()] = as;
    return 0;
}

int mmu_user_space_deactivate()
{
    UserAddressSpace* as = active_as[smp_cpu_id_fast()];
    if (!as) return 0;
    asm volatile("msr ttbr0_el1, %0\n isb" :: "r"((uint64_t)l1_table) : "memory");
    tlb_flush_all();
    active_as[smp_cpu_id_fast()] = 0;
    return 0;
}

int mmu_user_space_destroy(int pid)
{
    UserAddressSpace* as = space_find(pid);
    if (!as) return -1;
    space_unregister(as);
    if (read_ttbr0() == (uint64_t)as->l1) mmu_ensure_kernel_space();    /* this CPU must not keep running on tables about to be freed */
    int elsewhere = as_active_elsewhere(as);
    for (int c = 0; c < SMP_MAX_CPUS; ++c) if (active_as[c] == as) active_as[c] = 0;
    space_free(as);
    if (elsewhere) tlb_flush_all_is(); else tlb_flush_all();
    return 0;
}

/* Swap an exec'd image in: destroy `pid`'s space and give its identity to the already-built replacement. */
int mmu_user_space_replace(int pid, int built_pid)
{
    UserAddressSpace* fresh = space_find(built_pid);
    if (!fresh || !fresh->ready) return -1;
    if (space_find(pid)) (void)mmu_user_space_destroy(pid);
    fresh->pid = pid;
    return 0;
}

int mmu_user_space_clone(int src_pid, int dst_pid)
{
    UserAddressSpace* s = space_find(src_pid);
    if (!s || !s->ready) return -1;
    if (space_find(dst_pid)) return -2;
    UserAddressSpace* d = space_alloc(dst_pid);
    if (!d) return -3;

    d->vma = (Vma*)kmalloc((uint64_t)(s->capvma ? s->capvma : 16) * sizeof(Vma));
    if (!d->vma) { space_free(d); return -3; }
    d->capvma = s->capvma ? s->capvma : 16;
    for (int i = 0; i < s->nvma; ++i) d->vma[i] = s->vma[i];
    d->nvma = s->nvma;
    d->brk_start = s->brk_start; d->brk_cur = s->brk_cur; d->mmap_top = s->mmap_top;
    d->stack_top = s->stack_top; d->stack_lo = s->stack_lo; d->image_end = s->image_end;

    for (int i = 0; i < s->nvma; ++i)
    {
        const Vma* v = &s->vma[i];
        uint64_t va = v->start;
        uint64_t* e;
        while ((e = pte_next(s, &va, v->end)) != 0)
        {
            uint64_t* de = pte_walk(d, va, 1);
            if (!de) { space_free(d); return -3; }
            uint64_t pte = *e;
            uint64_t frame = pte & PTE_ADDR_MASK;
            if (page_ref((void*)frame) != 0) { space_free(d); return -5; }
            if (!(v->flags & VMA_SHARED) && (v->prot & USER_W))
            {
                pte = make_pte(frame, v->prot, 1);          /* both sides read-only until the first write */
                *e = pte;
                clean_entry(e);
            }
            *de = pte;
            clean_entry(de);
            va += MMU_PAGE_SIZE;
        }
    }
    d->ready = 1;
    if (space_register(d) != 0) { space_free(d); return -3; }
    tlb_flush_as(s);                                        /* the parent's pages just became read-only */
    return 0;
}

/* ---- brk / mmap / munmap / mprotect ---- */

int mmu_user_brk(int pid, uint64_t new_brk, uint64_t* out)
{
    UserAddressSpace* as = space_find(pid);
    if (!as || as->brk_start == 0) return -1;
    if (new_brk == 0 || new_brk == as->brk_cur) { *out = as->brk_cur; return 0; }
    if (new_brk < as->brk_start || new_brk > MMU_USER_LIMIT) return -2;
    uint64_t want = page_align_up(new_brk);
    uint64_t have = page_align_up(as->brk_cur);
    if (want > have)
    {
        if (!vma_range_free(as, have, want)) return -3;
        Vma* hv = have > as->brk_start ? vma_find(as, have - 1) : 0;
        if (hv && (hv->flags & VMA_HEAP) && hv->end == have) hv->end = want;
        else if (vma_insert(as, vma_lower_bound(as, have), have, want, USER_R | USER_W, VMA_HEAP) != 0) return -3;
    }
    else if (want < have)
    {
        unmap_pages(as, want, have);
        tlb_flush_as(as);
        Vma* hv = vma_find(as, have - 1);
        if (hv && (hv->flags & VMA_HEAP) && hv->end == have)
        {
            if (want <= hv->start) vma_erase(as, (int)(hv - as->vma));
            else hv->end = want;
        }
    }
    as->brk_cur = new_brk;
    *out = new_brk;
    return 0;
}

/* heap + mmap bytes reserved (what RLIMIT_AS / RLIMIT_DATA limit) */
uint64_t mmu_user_vsize(int pid)
{
    UserAddressSpace* as = space_find(pid);
    if (!as) return 0;
    uint64_t total = 0;
    for (int i = 0; i < as->nvma; ++i)
        if (as->vma[i].flags & (VMA_HEAP | VMA_ANON | VMA_SHARED)) total += as->vma[i].end - as->vma[i].start;
    return total;
}

static int mmap_common(int pid, uint64_t length, uint32_t prot, uint32_t flags, uint64_t* out)
{
    UserAddressSpace* as = space_find(pid);
    if (!as || length == 0 || (prot & ~7U) != 0 || ((prot & USER_W) && (prot & USER_X))) return -1;
    uint64_t len = page_align_up(length);
    if (len < length || len > MMU_USER_LIMIT - MMU_USER_BASE) return -2;
    uint64_t base = vma_find_gap_topdown(as, len);
    if (base == 0) return -2;
    int idx = vma_lower_bound(as, base);
    if (vma_insert(as, idx, base, base + len, prot, flags) != 0) return -2;
    if (flags == VMA_ANON) vma_try_merge(as, idx);
    *out = base;
    return 0;
}

int mmu_user_mmap(int pid, uint64_t length, uint32_t prot, uint64_t* out)
{
    return mmap_common(pid, length, prot, VMA_ANON, out);
}

/* MAP_SHARED reservation: pages are backed explicitly by mmu_user_install_frame (no lazy faults). */
int mmu_user_mmap_shared(int pid, uint64_t length, uint32_t prot, uint64_t* out)
{
    if (prot & USER_X) return -1;
    return mmap_common(pid, length, prot, VMA_SHARED, out);
}

/* Back one page of a shared reservation with `frame` (the caller's page reference is consumed on success). */
int mmu_user_install_frame(int pid, uint64_t va, uint64_t frame)
{
    UserAddressSpace* as = space_find(pid);
    if (!as || va < MMU_USER_BASE || va >= MMU_USER_LIMIT || (va & (MMU_PAGE_SIZE - 1ULL)) != 0) return -1;
    Vma* v = vma_find(as, va);
    if (!v || !(v->flags & VMA_SHARED)) return -1;
    uint64_t* e = pte_walk(as, va, 1);
    if (!e || (*e & DESC_VALID)) return -1;
    *e = make_pte(frame, v->prot, 0);
    clean_entry(e);
    return 0;
}

int mmu_user_munmap(int pid, uint64_t addr, uint64_t length)
{
    UserAddressSpace* as = space_find(pid);
    if (!as || length == 0 || (addr & (MMU_PAGE_SIZE - 1ULL)) != 0) return -22;
    uint64_t end = page_align_up(addr + length);
    if (end < addr || addr < MMU_USER_BASE || end > MMU_USER_LIMIT || end <= addr) return -22;
    if (vma_split(as, addr) != 0 || vma_split(as, end) != 0) return -12;
    unmap_pages(as, addr, end);
    tlb_flush_as(as);
    int i = vma_lower_bound(as, addr);
    while (i < as->nvma && as->vma[i].start < end) vma_erase(as, i);
    return 0;
}

static void sync_code_frame(uint64_t frame)
{
    for (uint64_t o = 0; o < MMU_PAGE_SIZE; o += 64) asm volatile("dc cvau, %0" :: "r"(frame + o) : "memory");
}

int mmu_user_mprotect(int pid, uint64_t addr, uint64_t length, uint32_t prot)
{
    UserAddressSpace* as = space_find(pid);
    if (!as || length == 0 || (addr & (MMU_PAGE_SIZE - 1ULL)) != 0 ||
        (prot & ~7U) != 0 || ((prot & USER_W) && (prot & USER_X)))
        return -22;
    uint64_t end = page_align_up(addr + length);
    if (end < addr || addr < MMU_USER_BASE || end > MMU_USER_LIMIT || end <= addr) return -22;
    if (!vma_range_fully_mapped(as, addr, end)) return -12;
    if (vma_split(as, addr) != 0 || vma_split(as, end) != 0) return -12;

    int exec_added = 0;
    for (int i = vma_lower_bound(as, addr); i < as->nvma && as->vma[i].start < end; ++i)
    {
        Vma* v = &as->vma[i];
        v->prot = prot;
        uint64_t va = v->start;
        uint64_t* e;
        while ((e = pte_next(as, &va, v->end)) != 0)
        {
            uint64_t frame = *e & PTE_ADDR_MASK;
            int cow = (*e & PTE_COW) != 0;
            if ((prot & USER_W) && !(v->flags & VMA_SHARED) && page_refcount((void*)frame) > 1) cow = 1;   /* never write through a frame another process still maps */
            *e = make_pte(frame, prot, cow);
            clean_entry(e);
            if (prot & USER_X) { sync_code_frame(frame); exec_added = 1; }
            va += MMU_PAGE_SIZE;
        }
    }
    if (exec_added) asm volatile("dsb ish\n ic ialluis\n dsb ish\n isb" ::: "memory");
    tlb_flush_as(as);
    return 0;
}

int mmu_user_space_sync_code(int pid, uint64_t start, uint64_t end)
{
    UserAddressSpace* as = space_find(pid);
    if (!as || !as->ready || start < MMU_USER_BASE || end <= start || end > MMU_USER_LIMIT) return -1;
    uint64_t va = start & ~(MMU_PAGE_SIZE - 1ULL);
    uint64_t* e;
    while ((e = pte_next(as, &va, end)) != 0)
    {
        if (!(*e & DESC_UXN)) sync_code_frame(*e & PTE_ADDR_MASK);
        va += MMU_PAGE_SIZE;
    }
    asm volatile("dsb ish\n ic ialluis\n dsb ish\n isb" ::: "memory");
    return 0;
}

int mmu_user_space_stats(int pid, uint64_t* resident, uint64_t* shared, uint64_t* cow_faults, uint64_t* demand_faults)
{
    UserAddressSpace* as = space_find(pid);
    if (!as) return -1;
    uint64_t r = 0, sh = 0;
    for (int i = 0; i < as->nvma; ++i)
    {
        uint64_t va = as->vma[i].start;
        uint64_t* e;
        while ((e = pte_next(as, &va, as->vma[i].end)) != 0)
        {
            ++r;
            if (page_refcount((void*)(*e & PTE_ADDR_MASK)) > 1) ++sh;
            va += MMU_PAGE_SIZE;
        }
    }
    if (resident) *resident = r;
    if (shared) *shared = sh;
    if (cow_faults) *cow_faults = as->cow_faults;
    if (demand_faults) *demand_faults = as->demand_faults;
    return 0;
}

int mmu_user_space_vma(int pid, int index, uint64_t* start, uint64_t* end, uint32_t* prot, uint32_t* kind)
{
    UserAddressSpace* as = space_find(pid);
    if (!as || index < 0 || index >= as->nvma) return -1;
    if (start) *start = as->vma[index].start;
    if (end) *end = as->vma[index].end;
    if (prot) *prot = as->vma[index].prot;
    if (kind) *kind = as->vma[index].flags;
    return 0;
}

int mmu_user_space_layout(int pid, uint64_t* brk_start, uint64_t* brk_cur, uint64_t* mmap_top, uint64_t* stack_lo, uint64_t* stack_top, uint64_t* table_pages)
{
    UserAddressSpace* as = space_find(pid);
    if (!as) return -1;
    if (brk_start) *brk_start = as->brk_start;
    if (brk_cur) *brk_cur = as->brk_cur;
    if (mmap_top) *mmap_top = as->mmap_top;
    if (stack_lo) *stack_lo = as->stack_lo;
    if (stack_top) *stack_top = as->stack_top;
    if (table_pages) *table_pages = as->table_pages;
    return 0;
}

int mmu_user_space_count() { return nspaces; }

/* 1 when `va` lies in a MAP_SHARED mapping: futexes there are keyed by physical address so other processes find them. */
int mmu_user_space_is_shared(int pid, uint64_t va)
{
    UserAddressSpace* as = space_find(pid);
    if (!as) return 0;
    const Vma* v = vma_find(as, va);
    return v && (v->flags & VMA_SHARED) ? 1 : 0;
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
    uart_puts("User VA : 0x100000000 - 0x3FC0000000 (252 GiB per process, private page tables, demand paged)\r\n");
    uart_puts("Layout : image at its link address; heap, mmap area and stack (8 MiB, guard gap below) randomised per exec\r\n");
}
