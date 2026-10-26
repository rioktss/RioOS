# RioOS / MyKernel v1.9.0 P3 Test Matrix

## Automated in the release environment

- AArch64 kernel clean build with `-Wall -Wextra -Werror`
- User ELF clean build
- Kernel link with `ld.lld`
- No unresolved kernel ELF symbols
- Exception vector table exactly 2048 bytes and 2048-byte aligned
- Persistent filesystem host create/write/read/remount/delete test
- VFS host test (`tests/vfs_host_test.cpp`): writable `/home/user` startup, user can save there while root remains protected, `ls <dir>` with sizes, cwd path, `rm .` keeps cwd valid, VFS recursive remove path
- Filesystem host test also covers `fs_rename` (mv), mkdir-p, recursive remove, hidden files/default modes, UID/GID persistence/migration, owner/group/other permission denial, stat metadata, truncate zero-fill, and rejection of a corrupt `UINT64_MAX` inode size
- Network host test (`tests/net_host_test.cpp`): IPv4 parsing, ARP neighbor learning, UDP owner isolation, port conflicts, checksum handling, and DNS A lookup with a fake resolver peer
- File-descriptor host test (`tests/fd_host_test.cpp`): read/write/seek/append/truncate, access modes, per-PID table isolation, shared offsets across `dup`/`dup2`, pipe transfer/ring semantics, non-blocking empty reads, EOF, and `EPIPE`
- Process manager host test (`tests/process_host_test.cpp`): valid states, bounded EL0 metadata, unique RUNNING record, protected built-ins, zombie/exit-code lifecycle, and refusing to destroy a running process
- Scheduler/process integration host test (`tests/scheduler_process_host_test.cpp`): logical-current selection, removal/cleanup, zombie compaction, and fallback selection
- Framebuffer/UI host tests, including UI glyph antialiasing, BMP decoding bounds, wallpaper selection persistence, theme persistence, and UI composition
- Disk image/superblock static verification
- ELF AArch64/LOAD/W+X/entry-permission static verification; loader rejects files with no execute bit even under root
- ZIP integrity verification

## Required on the user's Termux/QEMU host

- Full QEMU boot and visual/input verification (not run in the build container unless `qemu-system-aarch64` is available). `run-ui.sh` wires QEMU-window input through VirtIO keyboard/tablet devices; real input behavior still depends on the installed QEMU build and host display backend.
- `selftest` command
- `threads` followed by `thread-demo` (cooperative switching only; IRQ remains off)
- `fds` and `syscall fd` (open/write/seek/read/close/dup/pipe smoke tests); userspace wrapper execution still requires QEMU validation
- `export DEMO=ok`, `echo $DEMO`, `unset DEMO`, `echo $DEMO`
- `cat /etc/example | wc -l` against an existing file (or create a test file first)
- `disk` command
- `mmu` and `level` commands
- `exec /bin/init.elf`
- `nano test.txt` at the default `/home/user` prompt and in `/storage/home`: type, Ctrl+S, Ctrl+X, then `cat test.txt` in the same directory; a write to system-owned `/` should report permission denied unless logged in as root
- `cp a.txt b.txt`, `cp a.txt /storage/home`, `mv b.txt c.txt`, `mv c.txt /storage/home`, `ls /storage`; prompt shows the current directory
- `nano` with no argument prints usage; `nano <directory>` is refused
- `net` and `ping 10.0.2.2`
- Create a file under `/storage/home`, reboot QEMU, and confirm it remains

The release does not claim these QEMU runtime checks were executed inside
the release build container when `qemu-system-aarch64` is unavailable.


## P1 known limitations

- Generic pipes are kernel primitives, but the shell pipeline parser still uses its existing in-memory command path; arbitrary ELF-to-ELF pipelines are not yet wired up.
- `fork()`, `wait()`, descriptor inheritance, and concurrent user ELF processes are not implemented. They require per-process EL0 address spaces and a real process context switch.
- Pipe I/O is deliberately non-blocking; full/empty conditions return negative errno-like values rather than suspending a thread.
- Timer IRQ remains disabled during normal boot; none of these tests enables it.


## P2 known limitations

- The root password `123` is compiled into the source for this development release. It only protects casual shell switching and is not secure against someone with the binary/source.
- Credentials are global to the current shell/syscall path, not stored in independent process credential structures; concurrent processes are not supported.
- The on-disk inode layout remains v1 and uses reserved bytes for UID/GID/tag. The migration path is regression-tested with host tests but still requires a real QEMU persistent-disk boot test.
- Full durability under sudden power loss, journaled transactions, symlinks/inodes beyond the current table, and open-file unlink semantics remain unimplemented.


## P3 networking limitations

- UDP handles use their own owner-PID-scoped namespace; they are not ordinary file descriptors yet.
- UDP receive and DNS lookup are polling-based. There is no IRQ-driven receive path.
- DNS supports A records only and uses a configured static DNS server (default `10.0.2.3`).
- TCP, DHCP, IPv6, fragment reassembly, and ARP cache expiry are not implemented.
- Actual QEMU networking tests (`ping 10.0.2.2`, `nslookup example.com`) must be run outside the host-test environment.
