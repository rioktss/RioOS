RioOS

RioOS is a simple ARM64 bare-metal operating system powered by MyKernel and designed to run on QEMU.

Features

- ARM64 kernel (MyKernel)
- UART & shell
- Filesystem
- Process management
- Scheduler
- System calls
- ELF userspace
- VirtIO block & network
- Generic Timer & GIC interrupt support

Running

bash build.sh

Then run RioOS using QEMU.

«🚧 Work in progress»

## P0 - Timer IRQ bring-up (staged)

Boot leaves the timer and CPU IRQs **off** (Stage A), so the shell behaves exactly as before.
Use the `irq` shell command to move through the stages one at a time:

| Stage | Command | Expected |
|-------|---------|----------|
| A | (default) | shell works, `irq` shows CPU IRQ masked |
| B | `irq oneshot` | `[PASS] stage B`, exactly one IRQ, timer disabled afterwards |
| C | `irq stagec [hz]` | periodic IRQ, scheduler ticks stay 0, shell still usable |
| D | `irq staged [hz]` | periodic IRQ + scheduler tick increases, shell still usable |
| - | `irq selftest` | runs B, C, D and checks no nesting / no unhandled IRQ |
| - | `irq off` / `irq status` | stop timer and mask IRQ / show IRQ statistics |

Stage E (real context-switch preemption) is intentionally not enabled yet.
If a stage hangs, that stage is the first broken one: report its output.

Hardening done in this phase: GICC/GICD enable value 3 (valid in all GICv2 views),
spurious IDs counted without EOI, nesting/depth tracking, 64-bit CVAL timer re-arm
without drift or TVAL wrap, timer self-disables when it fires with no mode (storm guard),
and kernel/unhandled exceptions now print ESR/ELR/FAR/SPSR.


## P1 - Source layout

```
boot/      boot.S, exceptions.S (vectors)
kernel/    kernel, exceptions, interrupt, timer, syscall, shell, commands, string
drivers/   uart, keyboard, virtio_mmio/blk/net, storage
memory/    memory, mmu
process/   process, scheduler, elf, user, user_entry.S
fs/        fs, vfs, mkfs_myfs (host tool)
net/       netstack
userspace/ user_init.S, user_init.ld
include/   all headers (compiled with -Iinclude)
tests/  storage/  build.sh  linker.ld
```

Files were moved with `git mv` (history kept, nothing deleted). Object names in `build/` are unchanged.


## P2 - Memory (in progress)

- `memory/page_alloc.cpp`: bitmap physical page allocator (`allocate_page()` / `free_page()`),
  4 KiB aligned, zeroed on allocation, double-free and bad-address rejection, out-of-memory returns 0.
  Pool: 4 MiB carved from the kernel heap at boot. Shell: `pages`; `selftest` has a `[PASS] page allocator` line.
- `kernel/fault.cpp`: decodes ESR into instruction/data abort, read/write, translation/permission/access
  fault + level. Kernel and EL0 faults now print `[FAULT] ...` with ELR/FAR/ESR/SPSR before the existing handling.
- `tests/mem_host_test.cpp` covers both on the host (runs inside `build.sh`).
- Existing `kmalloc` bump heap and `mmu_user_pointer_ok` are unchanged.
- Not done yet: per-page permission split (W^X), user page tables per process, heap `kfree`.


## P3 - Syscalls (in progress)

New syscalls (real implementations, EL0 pointers validated with `mmu_user_pointer_ok`):

| # | Name | Behaviour |
|---|------|-----------|
| 8 | `SYS_WRITE_BUF(buf,len)` | writes `len` bytes (<= 4096) to the console; -14 bad pointer, -22 too long |
| 9 | `SYS_SLEEP_MS(ms)` | sleeps `ms` (<= 10000) |
| 10 | `SYS_READ(buf,len)` | non-blocking console read into a *writable* user buffer, returns bytes read |
| 11 | `SYS_TIME_MS` | milliseconds since boot |

Fixed: `SYS_UNAME` previously only checked the destination for *read* access; it now requires write access.
`include/user_api.h` gained `user_write/user_read/user_sleep_ms/user_time_ms` wrappers (old API unchanged).
`tests/syscall_host_test.cpp` exercises the dispatcher with a fake user memory window.

Deferred (need per-process contexts / fd table, planned with the P4 scheduler): fork, exec, wait, yield, open, close.


## P4 - Diagnostics (part 1)

- `include/diag.h`, `kernel/diag.cpp`: `klog(level, msg)` with runtime level, `PANIC(msg)`, `KASSERT(cond)`
  (prints message + file:line, masks IRQs, halts). Host-tested in `tests/diag_host_test.cpp`.
- New shell commands: `tasks`, `mount`, `stats`, `log [0-3]` (existing `ps`, `mem`, `uptime`, `uname`, `ls`, `cat`, `echo` unchanged).
- Not done (needs QEMU to validate safely): preemptive context switch, sleep/wakeup queues, priorities/aging,
  fs permissions/caching, sockets/UDP/TCP changes, driver framework.
