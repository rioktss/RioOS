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

#ifndef MMU_H
#define MMU_H

#include "types.h"


#define MMU_PAGE_SIZE        4096ULL
/* Each process owns 252 GiB of user address space (L1[4]..L1[254] of its private translation tree). The kernel identity map
   (devices, RAM, PCI ECAM) lives in the other L1 slots. */
#define MMU_USER_BASE        0x0000000100000000ULL
#define MMU_USER_LIMIT       0x0000003FC0000000ULL
#define MMU_USER_STACK_TOP   MMU_USER_LIMIT   /* upper bound of any user address (the real stack top is randomised per process) */

/* Permission bits accepted by mmu_user_set_range(). */
#define MMU_USER_READ  1U
#define MMU_USER_WRITE 2U
#define MMU_USER_EXEC  4U

void mmu_init();
void mmu_kernel_guard_page(uint64_t addr, int on);   /* unmap (on) / remap (off) one page of the identity-mapped kernel RAM: stack guard */
int mmu_enabled();
uint64_t mmu_read_sctlr();
uint64_t mmu_read_ttbr0();
uint64_t mmu_read_tcr();
void mmu_status();

int mmu_user_pointer_ok(uint64_t address, uint64_t length, int write);
uint64_t mmu_user_stack_top();                 /* stack top of the address space active on this CPU */
uint64_t mmu_user_space_stack_top(int pid);

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
int mmu_user_space_is_loaded(int pid);   /* this CPU's TTBR0 really is pid's table (SMP-safe) */
void mmu_ensure_kernel_space();           /* this CPU runs on the kernel table, whatever other CPUs did */


/* Demand paging / copy-on-write / dynamic memory (per active address space). */
int mmu_user_fault(uint64_t far, int is_write);
int mmu_user_fault_ex(uint64_t far, int is_write, int is_exec);
int mmu_user_space_clone(int src_pid, int dst_pid);
int mmu_user_space_replace(int pid, int built_pid);
int mmu_user_brk(int pid, uint64_t new_brk, uint64_t* out);
uint64_t mmu_user_vsize(int pid);          /* heap + mmap bytes reserved (RLIMIT_AS / RLIMIT_DATA) */
int mmu_user_mmap(int pid, uint64_t length, uint32_t prot, uint64_t* out);
int mmu_user_mmap_shared(int pid, uint64_t length, uint32_t prot, uint64_t* out);
int mmu_user_install_frame(int pid, uint64_t va, uint64_t frame);
int mmu_user_munmap(int pid, uint64_t addr, uint64_t length);
int mmu_user_mprotect(int pid, uint64_t addr, uint64_t length, uint32_t prot);
int mmu_user_space_stats(int pid, uint64_t* resident, uint64_t* shared, uint64_t* cow_faults, uint64_t* demand_faults);
/* Mapping inspection (the `maps` command): kind bits 1 anonymous, 2 shared, 4 stack, 8 heap, 16 image. */
int mmu_user_space_vma(int pid, int index, uint64_t* start, uint64_t* end, uint32_t* prot, uint32_t* kind);
int mmu_user_space_layout(int pid, uint64_t* brk_start, uint64_t* brk_cur, uint64_t* mmap_top, uint64_t* stack_lo, uint64_t* stack_top, uint64_t* table_pages);
int mmu_user_space_count();
int mmu_user_space_is_shared(int pid, uint64_t va);   /* inside a MAP_SHARED mapping */

#endif
