#ifndef MMU_H
#define MMU_H

#include "types.h"


#define MMU_PAGE_SIZE        4096ULL
#define MMU_USER_BASE        0x47000000ULL
#define MMU_USER_LIMIT       0x47800000ULL
#define MMU_USER_STACK_BASE  0x47FF0000ULL /* 64 KiB mapped stack; lower pages remain a guard */
#define MMU_USER_STACK_TOP   0x48000000ULL

/* Permission bits accepted by mmu_user_set_range(). */
#define MMU_USER_READ  1U
#define MMU_USER_WRITE 2U
#define MMU_USER_EXEC  4U

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

/* P5: independent EL0 address spaces. They share kernel/device mappings but
   own private user-image and stack page tables plus allocated physical pages. */
int mmu_user_space_create(int pid);
int mmu_user_space_map(int pid, uint64_t start, uint64_t end, uint32_t flags);
int mmu_user_space_copy_to(int pid, uint64_t destination, const void* source, uint64_t length);
void* mmu_user_space_pointer(int pid, uint64_t address);
int mmu_user_space_finish(int pid);
int mmu_user_space_activate(int pid);
int mmu_user_space_deactivate();
int mmu_user_space_destroy(int pid);
int mmu_user_space_sync_code(int pid, uint64_t start, uint64_t end);
int mmu_user_space_exists(int pid);
int mmu_user_space_active_pid();

#endif
