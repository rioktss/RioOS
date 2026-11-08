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

struct HeapBlock;
static HeapBlock* heap_head = 0;
static uint64_t heap_live = 0;
static uint64_t heap_allocs = 0;
static uint64_t heap_frees = 0;

static uint64_t align16(uint64_t value)
{
    return (value + 15ULL) & ~15ULL;
}

void memory_init()
{
    heap_start = align16((uint64_t)__heap_start);
    heap_current = heap_start;
    heap_head = 0;
    heap_live = 0;

    /* Keep a permanent guard before the reserved EL0 program window. */
    heap_end = FRAMEBUFFER_BASE;
    if (heap_end > USER_REGION_BASE - HEAP_GUARD_SIZE)
        heap_end = USER_REGION_BASE - HEAP_GUARD_SIZE;
    if (heap_end <= heap_start)
        heap_end = RAM_END - HEAP_GUARD_SIZE;

    uart_puts("Memory manager initialized.\r\n");
}

/* First-fit allocator over an address-ordered block list with coalescing.
   Every block carries a 32-byte header so kmalloc results stay 16-byte aligned. */
struct HeapBlock
{
    uint64_t size;      /* payload bytes */
    uint64_t free;      /* 1 = free, 0xA110C8ED = allocated (guard) */
    HeapBlock* next;
    uint64_t pad;
};

#define HEAP_ALLOCATED 0xA110C8EDULL
#define HEAP_HDR 32ULL


static void heap_lazy_init()
{
    if (heap_head || heap_end <= heap_start + 2 * HEAP_HDR) return;
    heap_head = (HeapBlock*)heap_start;
    heap_head->size = heap_end - heap_start - HEAP_HDR;
    heap_head->free = 1;
    heap_head->next = 0;
    heap_live = 0;
}

static uint64_t irq_mask_save()
{
#ifdef HOST_TEST
    return 0;
#else
    uint64_t d;
    asm volatile("mrs %0, daif\n msr daifset, #0x2\n isb" : "=r"(d) :: "memory");
    return d;
#endif
}
static void irq_mask_restore(uint64_t d)
{
#ifndef HOST_TEST
    asm volatile("msr daif, %0\n isb" :: "r"(d) : "memory");
#else
    (void)d;
#endif
}

void* kmalloc(uint64_t size)
{
    if (size == 0 || size > RAM_SIZE) return 0;
    uint64_t need = align16(size);
    uint64_t f = irq_mask_save();
    heap_lazy_init();
    for (HeapBlock* b = heap_head; b; b = b->next)
    {
        if (b->free != 1 || b->size < need) continue;
        if (b->size >= need + HEAP_HDR + 16)
        {
            HeapBlock* rest = (HeapBlock*)((uint64_t)b + HEAP_HDR + need);
            rest->size = b->size - need - HEAP_HDR;
            rest->free = 1;
            rest->next = b->next;
            b->next = rest;
            b->size = need;
        }
        b->free = HEAP_ALLOCATED;
        heap_live += b->size;
        ++heap_allocs;
        irq_mask_restore(f);
        return (void*)((uint64_t)b + HEAP_HDR);
    }
    irq_mask_restore(f);
    return 0;
}

void kfree(void* ptr)
{
    if (ptr == 0) return;
    uint64_t addr = (uint64_t)ptr;
    if (addr < heap_start + HEAP_HDR || addr >= heap_end || (addr & 15ULL) != 0) return;
    HeapBlock* b = (HeapBlock*)(addr - HEAP_HDR);
    uint64_t f = irq_mask_save();
    if (b->free != HEAP_ALLOCATED) { irq_mask_restore(f); return; } /* double free / bad pointer */
    b->free = 1;
    heap_live -= b->size;
    ++heap_frees;
    /* Coalesce with every adjacent free block (list is address ordered). */
    for (HeapBlock* c = heap_head; c; c = c->next)
    {
        while (c->free == 1 && c->next && c->next->free == 1 &&
               (uint64_t)c + HEAP_HDR + c->size == (uint64_t)c->next)
        {
            c->size += HEAP_HDR + c->next->size;
            c->next = c->next->next;
        }
    }
    irq_mask_restore(f);
}

void* kcalloc(uint64_t count, uint64_t size)
{
    if (size != 0 && count > RAM_SIZE / size) return 0;
    uint64_t total = count * size;
    uint8_t* p = (uint8_t*)kmalloc(total);
    if (p) for (uint64_t i = 0; i < total; ++i) p[i] = 0;
    return p;
}

void* krealloc(void* ptr, uint64_t size)
{
    if (ptr == 0) return kmalloc(size);
    if (size == 0) { kfree(ptr); return 0; }
    HeapBlock* b = (HeapBlock*)((uint64_t)ptr - HEAP_HDR);
    if (b->free != HEAP_ALLOCATED) return 0;
    if (b->size >= size) return ptr;
    uint8_t* n = (uint8_t*)kmalloc(size);
    if (!n) return 0;
    const uint8_t* o = (const uint8_t*)ptr;
    for (uint64_t i = 0; i < b->size; ++i) n[i] = o[i];
    kfree(ptr);
    return n;
}

uint64_t heap_alloc_count() { return heap_allocs; }
uint64_t heap_free_count() { return heap_frees; }

uint64_t memory_total() { return RAM_SIZE; }
uint64_t heap_total() { return heap_end > heap_start ? heap_end - heap_start : 0; }
uint64_t heap_used() { return heap_live; }
uint64_t heap_free()
{
    uint64_t total = heap_total();
    uint64_t used = heap_used();
    return used >= total ? 0 : total - used;
}
