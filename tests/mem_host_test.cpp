#include "page_alloc.h"
#include "fault.h"
#include <stdio.h>
#include <stdlib.h>

/* uart_* are only referenced by fault_report(), which this test never calls. */
extern "C++" void uart_puts(const char*) {}
extern "C++" void uart_putc(char) {}

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (0)

int main()
{
    /* 16 pages + slack, deliberately unaligned base. */
    char* raw = (char*)malloc(17 * 4096 + 4096);
    CHECK(raw != 0);
    CHECK(page_alloc_init(raw + 123, 16 * 4096 + 4096) == 0);

    PageStats st;
    page_alloc_get_stats(&st);
    CHECK(st.total_pages >= 16 && st.total_pages <= 17);

    void* pages[32];
    uint64_t n = 0;

    while ((pages[n] = allocate_page()) != 0)
    {
        CHECK(((uint64_t)pages[n] & 4095ULL) == 0);
        for (int i = 0; i < 4096; ++i) CHECK(((char*)pages[n])[i] == 0);
        for (int i = 0; i < 4096; ++i) ((char*)pages[n])[i] = (char)0xAA;
        for (uint64_t j = 0; j < n; ++j) CHECK(pages[j] != pages[n]);
        ++n;
        CHECK(n < 32);
    }

    CHECK(n == st.total_pages);
    page_alloc_get_stats(&st);
    CHECK(st.free_pages == 0 && st.failed_allocs >= 1);

    CHECK(free_page(pages[3]) == PAGE_FREE_OK);
    CHECK(free_page(pages[3]) == PAGE_FREE_DOUBLE);
    CHECK(free_page((char*)pages[4] + 8) == PAGE_FREE_BAD_ADDRESS);
    CHECK(free_page(0) == PAGE_FREE_BAD_ADDRESS);
    CHECK(free_page((void*)8) == PAGE_FREE_BAD_ADDRESS);

    void* again = allocate_page();
    CHECK(again == pages[3]);
    for (int i = 0; i < 4096; ++i) CHECK(((char*)again)[i] == 0);

    for (uint64_t i = 0; i < n; ++i)
        CHECK(free_page(i == 3 ? again : pages[i]) == PAGE_FREE_OK);

    page_alloc_get_stats(&st);
    CHECK(st.free_pages == st.total_pages);
    CHECK(st.rejected_frees == 4);

    CHECK(page_alloc_init(0, 4096) == -1);

    /* Fault decoding. */
    FaultInfo f;
    fault_decode((0x24ULL << 26) | (1ULL << 6) | 0x0FULL, &f);   /* EL0 write, perm fault L3 */
    CHECK(f.kind == FAULT_DATA_ABORT && f.from_user && f.is_write && f.level == 3);
    fault_decode((0x25ULL << 26) | 0x05ULL, &f);                 /* EL1 read, translation L1 */
    CHECK(f.kind == FAULT_DATA_ABORT && !f.from_user && !f.is_write && f.level == 1);
    fault_decode((0x20ULL << 26) | 0x0DULL, &f);                 /* EL0 ifetch, permission L1 */
    CHECK(f.kind == FAULT_INSTRUCTION_ABORT && f.from_user && f.level == 1);
    fault_decode(0x15ULL << 26, &f);                             /* SVC */
    CHECK(f.kind == FAULT_OTHER);

    free(raw);
    printf("MEM HOST TEST: PASS\n");
    return 0;
}
