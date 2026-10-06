#ifndef ELF_H
#define ELF_H

#include "types.h"

int elf_load(const char* path, uint64_t* entry_out);
void elf_status();

#endif
