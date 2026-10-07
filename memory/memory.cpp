#include "memory.h"
#include "uart.h"

#define RAM_START       0x40000000ULL
#define RAM_SIZE        (128ULL * 1024ULL * 1024ULL)
#define RAM_END         (RAM_START + RAM_SIZE)
#define USER_REGION_BASE 0x47000000ULL
#define FRAMEBUFFER_BASE 0x46800000ULL
#define HEAP_GUARD_SIZE  0x00100000ULL

extern "C" char __heap_start[];

static uint64_t heap_start = 0;
static uint64_t heap_current = 0;
static uint64_t heap_end = 0;

static uint64_t align16(uint64_t value)
{
    return (value + 15ULL) & ~15ULL;
}

void memory_init()
{
    heap_start = align16((uint64_t)__heap_start);
    heap_current = heap_start;

    /* Keep a permanent guard before the reserved EL0 program window. */
    heap_end = FRAMEBUFFER_BASE;
    if (heap_end > USER_REGION_BASE - HEAP_GUARD_SIZE)
        heap_end = USER_REGION_BASE - HEAP_GUARD_SIZE;
    if (heap_end <= heap_start)
        heap_end = RAM_END - HEAP_GUARD_SIZE;

    uart_puts("Memory manager initialized.\r\n");
}

void* kmalloc(uint64_t size)
{
    if (size == 0)
        return 0;
    if (size > 0xFFFFFFFFFFFFF000ULL - heap_current)
        return 0;

    uint64_t start = align16(heap_current);
    uint64_t end = start + size;
    if (end < start || end > heap_end)
        return 0;

    heap_current = align16(end);
    return (void*)start;
}

uint64_t memory_total() { return RAM_SIZE; }
uint64_t heap_total() { return heap_end > heap_start ? heap_end - heap_start : 0; }
uint64_t heap_used() { return heap_current > heap_start ? heap_current - heap_start : 0; }
uint64_t heap_free()
{
    uint64_t total = heap_total();
    uint64_t used = heap_used();
    return used >= total ? 0 : total - used;
}
