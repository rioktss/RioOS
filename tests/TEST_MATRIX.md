# MyKernel v1.5.0 Test Matrix

## Automated in the release environment

- AArch64 kernel clean build with `-Wall -Wextra -Werror`
- User ELF clean build
- Kernel link with `ld.lld`
- No unresolved kernel ELF symbols
- Exception vector table exactly 2048 bytes and 2048-byte aligned
- Persistent filesystem host create/write/read/remount/delete test
- VFS host test (`tests/vfs_host_test.cpp`): `ls <dir>` with sizes, cwd path, `rm .` keeps cwd valid, VFS recursive remove path
- Filesystem host test also covers `fs_rename` (mv), mkdir-p, recursive remove, hidden files and default modes
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
- `nano test.txt` in `/` and in `/storage/home`: type, Ctrl+S, Ctrl+X, then `cat test.txt` in the same directory
- `cp a.txt b.txt`, `cp a.txt /storage/home`, `mv b.txt c.txt`, `mv c.txt /storage/home`, `ls /storage`; prompt shows the current directory
- `nano` with no argument prints usage; `nano <directory>` is refused
- `net` and `ping 10.0.2.2`
- Create a file under `/storage/home`, reboot QEMU, and confirm it remains

The release does not claim these QEMU runtime checks were executed inside
the release build container when `qemu-system-aarch64` is unavailable.
