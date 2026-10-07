#ifndef PAGE_ALLOC_H
#define PAGE_ALLOC_H

#include "types.h"

#define PAGE_SIZE_BYTES 4096ULL
#define PAGE_ALLOC_MAX_PAGES 4096U

/* Result codes for page_free(). */
#define PAGE_FREE_OK           0
#define PAGE_FREE_BAD_ADDRESS (-1)  /* null, unaligned or outside the pool */
#define PAGE_FREE_DOUBLE      (-2)  /* page was not allocated              */

struct PageStats
{
    uint64_t total_pages;
    uint64_t free_pages;
    uint64_t allocs;
    uint64_t frees;
    uint64_t failed_allocs;     /* out-of-memory */
    uint64_t rejected_frees;    /* bad address or double free */
};

/* Manage `pages` 4 KiB pages starting at `base` (rounded up to a page
   boundary). Returns 0 on success, -1 on invalid arguments. */
int page_alloc_init(void* base, uint64_t size_bytes);

/* Returns a zeroed, 4 KiB-aligned page or 0 when out of memory. */
void* allocate_page();

/* Returns a PAGE_FREE_* code. Never corrupts allocator state. */
int free_page(void* page);

void page_alloc_get_stats(PageStats* out);
int page_alloc_ready();

#endif
