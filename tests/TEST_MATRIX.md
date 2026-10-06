# MyKernel v1.5.0 Test Matrix

## Automated in the release environment

- AArch64 kernel clean build with `-Wall -Wextra -Werror`
- User ELF clean build
- Kernel link with `ld.lld`
- No unresolved kernel ELF symbols
- Exception vector table exactly 2048 bytes and 2048-byte aligned
- Persistent filesystem host create/write/read/remount/delete test
- Network parser host test (`tests/net_host_test.cpp`) (valid/invalid IPv4 input)
- Disk image/superblock static verification
- ELF AArch64/LOAD/W+X/entry-permission static verification
- ZIP integrity verification

## Required on the user's Termux/QEMU host

- Full QEMU boot
- `selftest` command
- `disk` command
- `mmu` and `level` commands
- `exec /bin/init.elf`
- `net` and `ping 10.0.2.2`
- Create a file under `/storage/home`, reboot QEMU, and confirm it remains

The release does not claim these QEMU runtime checks were executed inside
the release build container when `qemu-system-aarch64` is unavailable.
