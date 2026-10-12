# P0 Kernel Hardening — RioOS 1.6.1

This increment focuses on safety invariants in the existing single-core kernel. It deliberately does **not** enable the Generic Timer IRQ or advertise full process preemption.

## Changes in this increment

- Process-table reads and writes are protected by saving/restoring the CPU IRQ mask, and host tests use a no-assembly test shim.
- A process record cannot be configured outside the reserved EL0 virtual window.
- Process state values are validated, only one process record can be marked `RUNNING` at a time, PIDs do not wrap through signed overflow, and the built-in kernel/shell records cannot be destroyed or marked as exited.
- A process marked `RUNNING` cannot be reclaimed directly; it must first leave the running state or become a zombie.
- The synchronous EL0 `exec` path restores the kernel record's running metadata when EL0 returns.
- MMU user-page setup fails closed, validates page-permission flags, rejects unknown flags and write/execute mappings, and refuses to configure EL0 mappings when the MMU is disabled.
- The ELF loader accepts AArch64 `ET_EXEC`, validates load flags/alignment, rejects PT_LOAD ranges that share a page (permissions are page-granular), clears stale loader status on failure, and resets user mappings if activation fails.
- User path/string syscall scans use integer-address overflow guards before dereferencing user-provided pointers.
- Added `tests/process_host_test.cpp` and wired it into `build.sh` and release verification.

## Explicit limits (not completed by this patch)

The process scheduler remains a **logical PID/state scheduler**; its tick does not switch the active CPU context. `process/thread.cpp` provides cooperative **kernel-thread** switching, but it is not a per-process EL0 context switch. Each EL0 execution still uses one reserved user virtual window and runs synchronously until syscall-exit or a fatal exception. Per-process page tables/address spaces, separately saved EL0 register frames, a generic process context switch, and real process-level preemptive multitasking remain future work.

Timer IRQ and CPU IRQ unmasking stay disabled in the normal boot path. Host tests validate helper invariants but cannot replace a QEMU boot/interactivity test.

## Verification

Run from the repository root:

```sh
bash build.sh
```

The build compiles the AArch64 kernel, runs host tests, checks ELF/link layout, checks unresolved symbols, and runs static release verification. Then separately boot the resulting kernel in QEMU and test keyboard input, shell responsiveness, `/bin/init.elf`, process exit cleanup, and filesystem persistence.
