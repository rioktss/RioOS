# P4 — SMP, driver, and framebuffer foundations (RioOS 1.10.0)

## Included

- A small CPU-identification API based on `MPIDR_EL1.Aff0`.
- An explicit report of online CPU count and SMP state. The count remains one and SMP remains disabled until secondary-core boot, per-CPU stacks/state, interrupt routing, and synchronization requirements are implemented together.
- Acquire/release spinlock primitives using compiler atomics, with try-lock support. They are groundwork for future SMP work and are not yet used to claim that current drivers are SMP-safe.
- Framebuffer surface bounds checks, background-pixel clearing, cursor clamping, and text-row scrolling rather than clearing the entire surface when it reaches the bottom.
- A host-test-only framebuffer attachment hook, so rendering logic is testable without dereferencing physical addresses on the host.
- VirtIO-Net receive validation that rejects impossible device-reported lengths and returns an `EMSGSIZE`-style error rather than silently truncating Ethernet frames. Rejected but validly indexed RX buffers are recycled.
- `smp` and `fbinfo` shell diagnostics.
- GitHub Actions CI to perform a clean build, host regression tests, and release verification on pushes and pull requests.

## Safety limits / not included

- No secondary CPU is released. `smp_online_cpu_count()` stays one and `smp_is_enabled()` stays false.
- The new spinlocks are primitives only; current subsystems have not been audited as SMP-safe, and the locks are not used to imply that they are.
- The framebuffer code draws into a reserved RAM surface. The default QEMU runner does not attach a QEMU display device to that memory, so UART remains the real console. `fbinfo` states this explicitly.
- The software framebuffer is not a GUI/window manager and does not add mouse or graphics-device support.
- Timer IRQ remains disabled during normal boot; P4 does not enable preemption or CPU IRQs.
- CI and host tests do not replace interactive QEMU boot validation on a host with `qemu-system-aarch64`.

## Validation

`build.sh` now includes host tests for spinlock/SMP default-state contracts and framebuffer glyph/scroll behaviour. `tests/verify_release.py` checks the version, these test sources, network receive bounds, and that timer IRQ activation has not been added to the default boot path.
