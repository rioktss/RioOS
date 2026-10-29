# Timer IRQ (v1.13.0)

Up to 1.12.0 the generic-timer interrupt was kept off during normal boot and
was always masked while an EL0 program ran. From 1.13.0 it is a real system
tick.

## What changed

- `kernel_main()` (step 15) unmasks the timer PPI at the GIC, arms the physical timer for **100 Hz periodic**, enables the scheduler tick, and clears the CPU I-bit. The tick is therefore live in the kernel shell.
- `user_enter()` returns to EL0 with `SPSR_EL1 = 0x340` (EL0t, D/A/F masked, **I clear**) instead of `0x3C0`, so the tick keeps firing while `nano`, `calc` or any ELF runs. The lower-EL IRQ vector (`irq_el0_entry`) saves the full exception frame, calls `interrupt_dispatch()` and returns with `eret`.
- `user_execute()` no longer stops the timer. IRQs are masked only while the kernel builds the process address space and on the syscall/exit paths (the CPU masks them on exception entry); `user_execute()` restores the previous mask afterwards and re-arms the 100 Hz tick if something stopped it.
- The `irq` command gained `irq on`; `irq selftest`, `irq oneshot`, `irq stagec/staged` still run their staged tests and then restore the 100 Hz tick. `irq off` stops the tick until `irq on`.
- `calc` now prints `Timer IRQ is active (100 Hz).`

## What did not change

The scheduler tick only counts ticks (`scheduler_tick()`); there is still no
preemptive switching between processes, so EL0 programs run to completion from
the shell. Handlers never print or allocate.

## Verify on QEMU

```
irq status        # CPU IRQ: unmasked, Timer mode: periodic, Timer Hz: 100
sched             # "Sched ticks" keeps increasing
calc              # then, in another run, `irq status` after exiting: ticks continued during EL0
irq selftest      # 4/4 passed, then status shows the periodic tick again
```
These are runtime checks: the host tests cannot execute AArch64 interrupts.
