#ifndef MEMORY_H
#define MEMORY_H

#include "types.h"

void memory_init();

void* kmalloc(uint64_t size);
void kfree(void* ptr);
void* kcalloc(uint64_t count, uint64_t size);
void* krealloc(void* ptr, uint64_t size);
uint64_t heap_alloc_count();
uint64_t heap_free_count();

uint64_t memory_total();
uint64_t heap_total();
uint64_t heap_used();
uint64_t heap_free();

#endif