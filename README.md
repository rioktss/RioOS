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
