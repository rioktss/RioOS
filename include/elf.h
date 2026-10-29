#ifndef ELF_H
#define ELF_H

#include "types.h"

int elf_load(const char* path, uint64_t* entry_out);
int elf_load_for_process(const char* path, uint64_t* entry_out, int pid);
void elf_status();

#endif
