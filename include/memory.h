#ifndef MEMORY_H
#define MEMORY_H

#include "types.h"

void memory_init();

void* kmalloc(uint64_t size);

uint64_t memory_total();
uint64_t heap_total();
uint64_t heap_used();
uint64_t heap_free();

#endif