#include "elf.h"
#include "fs.h"
#include "mmu.h"
#include "uart.h"
#include "string.h"

#define EI_NIDENT       16
#define ELFCLASS64      2
#define ELFDATA2LSB     1
#define EV_CURRENT      1
#define EM_AARCH64      183
#define PT_LOAD         1
#define PF_X             1U
#define PF_W             2U
#define PF_R             4U
#define MAX_ELF_SIZE    (256ULL * 1024ULL)
#define MAX_PHDRS       32

struct __attribute__((packed)) Elf64_Ehdr
{
    uint8_t  ident[EI_NIDENT];
    uint16_t type;
    uint16_t machine;
    uint32_t version;
    uint64_t entry;
    uint64_t phoff;
    uint64_t shoff;
    uint32_t flags;
    uint16_t ehsize;
    uint16_t phentsize;
    uint16_t phnum;
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
};

struct __attribute__((packed)) Elf64_Phdr
{
    uint32_t type;
    uint32_t flags;
    uint64_t offset;
    uint64_t vaddr;
    uint64_t paddr;
    uint64_t filesz;
    uint64_t memsz;
    uint64_t align;
};

static uint8_t image[MAX_ELF_SIZE] __attribute__((aligned(16), section(".bss.elf_loader")));
static uint64_t last_entry = 0;
static uint64_t last_low = 0;
static uint64_t last_high = 0;
static int last_loaded = 0;
static char last_path[96];

static void print_hex(uint64_t value)
{
    static const char h[] = "0123456789ABCDEF";
    for (int i = 15; i >= 0; --i)
        uart_putc(h[(value >> (i * 4)) & 0xF]);
}

static int range_valid(uint64_t start, uint64_t size, uint64_t low, uint64_t high)
{
    if (start < low || start >= high)
        return 0;
    if (size > high - start)
        return 0;
    return 1;
}

int elf_load(const char* path, uint64_t* entry_out)
{
    if (path == 0 || entry_out == 0)
        return -1;

    uint64_t file_size = 0;
    if (fs_read_file(path, fs_get_root(), image, sizeof(image), &file_size) != 0)
    {
        uart_puts("ELF: file not found or too large.\r\n");
        return -2;
    }

    if (file_size < sizeof(Elf64_Ehdr))
    {
        uart_puts("ELF: file too small.\r\n");
        return -3;
    }

    const Elf64_Ehdr* eh = (const Elf64_Ehdr*)image;
    if (eh->ident[0] != 0x7F || eh->ident[1] != 'E' ||
        eh->ident[2] != 'L' || eh->ident[3] != 'F' ||
        eh->ident[4] != ELFCLASS64 || eh->ident[5] != ELFDATA2LSB ||
        eh->ident[6] != EV_CURRENT || eh->machine != EM_AARCH64 ||
        eh->version != EV_CURRENT || eh->ehsize < sizeof(Elf64_Ehdr) ||
        eh->phentsize != sizeof(Elf64_Phdr) || eh->phnum == 0 || eh->phnum > MAX_PHDRS)
    {
        uart_puts("ELF: invalid AArch64 executable.\r\n");
        return -4;
    }

    if (eh->phoff > file_size ||
        (uint64_t)eh->phnum * eh->phentsize > file_size - eh->phoff)
    {
        uart_puts("ELF: program header table outside file.\r\n");
        return -5;
    }

    mmu_user_prepare();

    uint64_t low = MMU_USER_LIMIT;
    uint64_t high = MMU_USER_BASE;
    int load_count = 0;
    int entry_executable = 0;

    for (uint16_t i = 0; i < eh->phnum; ++i)
    {
        const Elf64_Phdr* ph = (const Elf64_Phdr*)(image + eh->phoff + (uint64_t)i * eh->phentsize);
        if (ph->type != PT_LOAD)
            continue;
        ++load_count;

        if (ph->memsz < ph->filesz ||
            ph->offset > file_size || ph->filesz > file_size - ph->offset ||
            ph->vaddr < MMU_USER_BASE || ph->vaddr >= MMU_USER_LIMIT ||
            ph->memsz == 0 || ph->memsz > MMU_USER_LIMIT - ph->vaddr ||
            (ph->vaddr & (MMU_PAGE_SIZE - 1ULL)) != 0 ||
            (ph->offset & (MMU_PAGE_SIZE - 1ULL)) != 0 ||
            (ph->align != 0 && ph->align != 1 && ph->align < MMU_PAGE_SIZE))
        {
            uart_puts("ELF: invalid LOAD segment.\r\n");
            return -6;
        }

        if ((ph->flags & PF_W) && (ph->flags & PF_X))
        {
            uart_puts("ELF: W+X segment rejected.\r\n");
            return -7;
        }

        if (!range_valid(ph->vaddr, ph->memsz, MMU_USER_BASE, MMU_USER_LIMIT))
            return -8;

        for (uint64_t j = 0; j < ph->memsz; ++j)
            ((uint8_t*)ph->vaddr)[j] = 0;

        for (uint64_t j = 0; j < ph->filesz; ++j)
            ((uint8_t*)ph->vaddr)[j] = image[ph->offset + j];

        uint32_t mode = 0;
        if (ph->flags & PF_R) mode |= 1U;
        if (ph->flags & PF_W) mode |= 2U;
        if (ph->flags & PF_X) mode |= 4U;
        if ((mode & 1U) == 0 && (mode & 2U) == 0 && (mode & 4U) == 0)
            mode = 1U;

        uint64_t end = ph->vaddr + ph->memsz;

        /* Reject overlapping PT_LOAD memory ranges. A simple, non-overlapping
           segment layout keeps page permissions deterministic and prevents one
           segment from silently overriding another segment's attributes. */
        for (uint16_t j = 0; j < i; ++j)
        {
            const Elf64_Phdr* prev = (const Elf64_Phdr*)(image + eh->phoff + (uint64_t)j * eh->phentsize);
            if (prev->type != PT_LOAD)
                continue;
            uint64_t prev_end = prev->vaddr + prev->memsz;
            if (ph->vaddr < prev_end && prev->vaddr < end)
            {
                uart_puts("ELF: overlapping LOAD segments rejected.\r\n");
                return -13;
            }
        }

        if (mmu_user_set_range(ph->vaddr, end, mode) != 0)
        {
            uart_puts("ELF: page permission setup failed.\r\n");
            return -9;
        }

        if ((ph->flags & PF_X) != 0 &&
            eh->entry >= ph->vaddr && eh->entry < end)
            entry_executable = 1;

        if (ph->vaddr < low) low = ph->vaddr;
        if (end > high) high = end;
    }

    if (load_count == 0 || low >= high)
    {
        uart_puts("ELF: no loadable segments.\r\n");
        return -10;
    }

    if (eh->entry < low || eh->entry >= high || (eh->entry & 3ULL) != 0)
    {
        uart_puts("ELF: entry point outside loaded image.\r\n");
        return -11;
    }

    if (!entry_executable)
    {
        uart_puts("ELF: entry point is not executable.\r\n");
        return -14;
    }

    /* Give EL0 the private stack and make instruction fetches coherent. */
    if (mmu_user_finish() != 0)
        return -12;

    uint64_t flush_end = high > low ? high : low + MMU_PAGE_SIZE;
    uint64_t begin = low & ~63ULL;
    uint64_t end = (flush_end + 63ULL) & ~63ULL;
    for (uint64_t p = begin; p < end; p += 64ULL)
        asm volatile("dc cvau, %0" :: "r"(p) : "memory");
    asm volatile("dsb ish" ::: "memory");
    for (uint64_t p = begin; p < end; p += 64ULL)
        asm volatile("ic ivau, %0" :: "r"(p) : "memory");
    asm volatile("dsb ish\nisb" ::: "memory");

    str_copy(last_path, path, sizeof(last_path));
    last_entry = eh->entry;
    last_low = low;
    last_high = high;
    last_loaded = 1;
    *entry_out = eh->entry;
    return 0;
}

void elf_status()
{
    uart_puts("ELF loader status\r\n-----------------\r\n");
    uart_puts("Loaded : "); uart_puts(last_loaded ? "yes\r\n" : "no\r\n");
    if (!last_loaded) return;
    uart_puts("File   : "); uart_puts(last_path); uart_puts("\r\n");
    uart_puts("Range  : 0x"); print_hex(last_low);
    uart_puts(" - 0x"); print_hex(last_high); uart_puts("\r\n");
    uart_puts("Entry  : 0x"); print_hex(last_entry); uart_puts("\r\n");
}
