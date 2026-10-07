#include "page_alloc.h"

/* Bitmap page allocator: 1 bit per page (1 = allocated). The metadata lives
   outside the managed pool, so a stray write into a free page cannot corrupt
   the allocator. */

static uint8_t bitmap[PAGE_ALLOC_MAX_PAGES / 8U];
static uint64_t pool_base = 0;
static uint64_t pool_pages = 0;
static uint64_t hint = 0;
static PageStats stats;
static int ready = 0;

static int bit_get(uint64_t index)
{
    return (bitmap[index >> 3] >> (index & 7U)) & 1U;
}

static void bit_set(uint64_t index)
{
    bitmap[index >> 3] = (uint8_t)(bitmap[index >> 3] | (1U << (index & 7U)));
}

static void bit_clear(uint64_t index)
{
    bitmap[index >> 3] = (uint8_t)(bitmap[index >> 3] & ~(1U << (index & 7U)));
}

int page_alloc_init(void* base, uint64_t size_bytes)
{
    if (base == 0)
        return -1;

    uint64_t start = ((uint64_t)base + (PAGE_SIZE_BYTES - 1ULL)) &
                     ~(PAGE_SIZE_BYTES - 1ULL);
    uint64_t end = (uint64_t)base + size_bytes;

    if (end < (uint64_t)base || end <= start)
        return -1;

    uint64_t pages = (end - start) / PAGE_SIZE_BYTES;

    if (pages == 0)
        return -1;

    if (pages > PAGE_ALLOC_MAX_PAGES)
        pages = PAGE_ALLOC_MAX_PAGES;

    for (uint64_t i = 0; i < sizeof(bitmap); ++i)
        bitmap[i] = 0;

    pool_base = start;
    pool_pages = pages;
    hint = 0;

    stats.total_pages = pages;
    stats.free_pages = pages;
    stats.allocs = 0;
    stats.frees = 0;
    stats.failed_allocs = 0;
    stats.rejected_frees = 0;

    ready = 1;
    return 0;
}

void* allocate_page()
{
    if (!ready || stats.free_pages == 0)
    {
        ++stats.failed_allocs;
        return 0;
    }

    for (uint64_t n = 0; n < pool_pages; ++n)
    {
        uint64_t index = (hint + n) % pool_pages;

        if (bit_get(index))
            continue;

        bit_set(index);
        hint = index + 1ULL;
        --stats.free_pages;
        ++stats.allocs;

        volatile uint64_t* p = (volatile uint64_t*)(pool_base + index * PAGE_SIZE_BYTES);

        for (uint64_t w = 0; w < PAGE_SIZE_BYTES / 8ULL; ++w)
            p[w] = 0;

        return (void*)p;
    }

    ++stats.failed_allocs;
    return 0;
}

int free_page(void* page)
{
    uint64_t address = (uint64_t)page;

    if (!ready || address < pool_base ||
        (address & (PAGE_SIZE_BYTES - 1ULL)) != 0 ||
        address >= pool_base + pool_pages * PAGE_SIZE_BYTES)
    {
        ++stats.rejected_frees;
        return PAGE_FREE_BAD_ADDRESS;
    }

    uint64_t index = (address - pool_base) / PAGE_SIZE_BYTES;

    if (!bit_get(index))
    {
        ++stats.rejected_frees;
        return PAGE_FREE_DOUBLE;
    }

    bit_clear(index);
    ++stats.free_pages;
    ++stats.frees;

    if (index < hint)
        hint = index;

    return PAGE_FREE_OK;
}

void page_alloc_get_stats(PageStats* out)
{
    if (out != 0)
        *out = stats;
}

int page_alloc_ready()
{
    return ready;
}
