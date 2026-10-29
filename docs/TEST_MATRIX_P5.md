# RioOS P5 test matrix

| Area | Test | Expected result |
|---|---|---|
| Build | `bash linux/build.sh --clean` | AArch64 kernel and userspace ELF build with warnings treated as errors |
| PCB context | `tests/process_host_test.cpp` | Saves/loads all 31 GPRs, ELR, SPSR, and user stack pointer; clears context on destroy |
| Process/scheduler regression | `tests/scheduler_process_host_test.cpp` | Existing metadata scheduler behavior remains intact |
| MMU contracts | `tests/verify_release.py` | Confirms per-PID address-space functions, physical frame ownership, process-aware ELF loader, activation/deactivation and P5 docs |
| Memory ownership | QEMU runtime | Loading an ELF allocates private segment and stack frames; process exit releases them |
| Legacy ELF check | Existing shell self-test | Legacy diagnostic loader remains available and does not replace process-aware `exec` path |
| UI/filesystem/network | Existing regression suite | Existing shell, UI/RPK, filesystem, networking and file descriptors still build and test |

The host suite cannot execute privileged AArch64 TTBR0 operations. QEMU runtime validation is required before enabling interrupt-driven scheduling or claiming full EL0 process-to-process context switching.
