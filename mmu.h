#ifndef MMU_H
#define MMU_H

#include "types.h"


#define MMU_PAGE_SIZE        4096ULL
#define MMU_USER_BASE        0x47000000ULL
#define MMU_USER_LIMIT       0x47800000ULL
#define MMU_USER_STACK_BASE  0x47F00000ULL
#define MMU_USER_STACK_TOP   0x48000000ULL

void mmu_init();
int mmu_enabled();
uint64_t mmu_read_sctlr();
uint64_t mmu_read_ttbr0();
uint64_t mmu_read_tcr();
void mmu_status();

int mmu_user_prepare();
int mmu_user_set_range(uint64_t start, uint64_t end, uint32_t flags);
int mmu_user_finish();
int mmu_user_pointer_ok(uint64_t address, uint64_t length, int write);
uint64_t mmu_user_stack_top();

#endif
