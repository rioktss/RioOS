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
- Network host test (`tests/net_host_test.cpp`): IPv4 parsing, ARP neighbor learning, UDP owner isolation/checksums, DNS A lookup, and TCP handshake/send/receive/FIN with a fake peer
- File-descriptor host test (`tests/fd_host_test.cpp`): read/write/seek/append/truncate, access modes, per-PID table isolation, shared offsets across `dup`/`dup2`, pipe transfer/ring semantics, non-blocking empty reads, EOF, and `EPIPE`
- Process manager host test (`tests/process_host_test.cpp`): valid states, bounded EL0 metadata, unique RUNNING record, protected built-ins, zombie/exit-code lifecycle, and refusing to destroy a running process
- Scheduler/process integration host test (`tests/scheduler_process_host_test.cpp`): logical-current selection, removal/cleanup, zombie compaction, and fallback selection
- Framebuffer terminal host test (`console_host_test`): ANSI handling, scrollback, wheel/page scrolling, snap-to-bottom, font anti-aliasing, hostile escape sequences.
- Disk image/superblock static verification
- ELF AArch64/LOAD/W+X/entry-permission static verification; loader rejects files with no execute bit even under root
- ZIP integrity verification

## Required on the user's Termux/QEMU host

- Full QEMU boot and visual/input verification (not run in the build container unless `qemu-system-aarch64` is available): `linux/run-terminal.sh` - type, scroll with the mouse wheel, run `nano`/`calc`, check `irq status` ticks while a program runs.
- `selftest` command
- `threads` followed by `thread-demo` (cooperative switching only; the 100 Hz tick keeps running)
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
- `nslookup example.com` and `httpget neverssl.com /` to test real DNS and outbound plain HTTP
- Create a file under `/storage/home`, reboot QEMU, and confirm it remains

The release does not claim these QEMU runtime checks were executed inside
the release build container when `qemu-system-aarch64` is unavailable.


## P1 known limitations

- Generic pipes are kernel primitives, but the shell pipeline parser still uses its existing in-memory command path; arbitrary ELF-to-ELF pipelines are not yet wired up.
- `fork()`, `wait()`, descriptor inheritance, and concurrent user ELF processes are not implemented. They require per-process EL0 address spaces and a real process context switch.
- Pipe I/O is deliberately non-blocking; full/empty conditions return negative errno-like values rather than suspending a thread.
- The timer IRQ is active (100 Hz) during these tests; `irq selftest` restores it afterwards.


## P2 known limitations

- The root password `123` is compiled into the source for this development release. It only protects casual shell switching and is not secure against someone with the binary/source.
- Credentials are global to the current shell/syscall path, not stored in independent process credential structures; concurrent processes are not supported.
- The on-disk inode layout remains v1 and uses reserved bytes for UID/GID/tag. The migration path is regression-tested with host tests but still requires a real QEMU persistent-disk boot test.
- Full durability under sudden power loss, journaled transactions, symlinks/inodes beyond the current table, and open-file unlink semantics remain unimplemented.


## P3 networking limitations

- UDP handles use their own owner-PID-scoped namespace; they are not ordinary file descriptors yet.
- UDP receive and DNS lookup are polling-based. There is no IRQ-driven receive path.
- DNS supports A records only and uses a configured static DNS server (default `10.0.2.3`).
- TCP is a bounded outbound client: no passive listen/accept, congestion control, MSS negotiation, or out-of-order reassembly. HTTPS/TLS is not implemented; `httpget` is plain HTTP on port 80.
- DHCP, IPv6, fragment reassembly, and ARP cache expiry are not implemented.
- Actual QEMU networking tests (`ping 10.0.2.2`, `nslookup example.com`, `httpget neverssl.com /`) must be run outside the host-test environment.


## Extended release (preemptive EL0, processes, security, FS journal, drivers, network, SMP)

Host tests added to the build: `sha_host_test` (SHA-256, HMAC-SHA256, PBKDF2 vectors).

QEMU regression: `QEMU_BIOS=<pc-bios> python3 tests/qemu_regression.py [qemu-system-aarch64]` (34 checks, throw-away disk copy):
- session 1: `ktest` (28 checks: fork/COW, exec, wait, pipes, signals, mmap/brk/demand paging, privilege separation), `dltest` (dlopen/dlsym), shell scripting (`if/for/while/let/$?`), symlinks, remount ro/rw, journal, accounts (`su`, `useradd`, `/etc/passwd` PBKDF2 hashes)
- session 1b: reboot after `jtest` (journal commit without apply) must replay the transaction
- session 2: `-smp 4` secondary cores and `smp test`
- session 3: PCIe enumeration, NVMe and AHCI self-tests, root filesystem on NVMe
- session 4: DHCP, TCP server (`httpd`), IPv6 NDP/ICMPv6, ping against `tests/qemu_fake_peer.py` (needs QEMU `-netdev dgram`)

Round 2: host test `tls_host_test` (RFC 7748 X25519 vectors, NIST AES-GCM cases 2 and 4, RFC 5869 HKDF extract); QEMU sessions added:
- session 5: redirects, wildcards, `$()`, functions, `/tmp` volatility, `free`, >64 inodes
- session 6: xHCI + usb-kbd + usb-mouse via the QEMU monitor (`sendkey`, `mouse_move`)
- session 7: `tests/qemu_fake_peer2.py` TCP server (HTTP, echo, RST) and relay to `tests/tls_test_server.py` (real OpenSSL TLS 1.3): `dhcp renew`, `wget`, `nc`, `httpsget`, `wget https://`
- session 8 (round 3): multi-mount VFS with a second NVMe and AHCI disk, tmpfs, `mkfs`, `mount/umount/df`, cross-mount `mv`/`cp`, busy checks, persistence across reboot
- session 7 extended: IPv6 `udp6 send`, `nc` and `nc -z` over v6, `nc -l` accepting a v6 client

Round 5: host tests `softfloat_host_test` (bit-exact vs hardware), `libm_host_test`, `deflate_host_test` (vs zlib) run from build.sh; QEMU sessions added:
- session 11: `case`/`until`, echo quoting, `cat` pipe, Tab completion of arguments, `diff` (normal/-u/-q/-s/errors), `vi` (insert, yank/put, undo, search, `:s`, `:w`, `.`, `ZZ`, `-R`), new help/man entries
- session 12: gzip/gunzip/zcat and tar (tar + tgz) exchanged with host Python through a FAT32 image (both directions)
- session 13: `pkg` (build/update/search/install with dependency/verify/owner/remove protection) and `cc` (`tests/cprog/*.c`: language features, argv, runtime faults, compile errors, `-c -S -o -x`, scanf, step limit)
- libctest expects 54/54

Round 6: host syscall test extended for syscalls 60-64; QEMU session added:
- session 14: cross-mount symlinks (absolute/relative, directories, pwd), `stat/-L`, `find -type l`, `readlink -f`, `realpath`, `cp -P`, tar symlinks; `lntest` (15 userspace symlink checks); `proctest` (46 checks: fork/wait, execve with argv[0]/env, popen/system/posix_spawn, signal handlers/masks/alarm/pause/kill/SIGCHLD, exec handler reset, FD_CLOEXEC, shared anonymous and file mmap incl. cross-process visibility and write-back)

Round 7: QEMU session added:
- session 15: `thrtest` (31 pthread checks: create/join, mutex, cond, rwlock, barrier, semaphores, keys, cancel, pthread_kill, deadlock), `sigtest` (36 checks: siginfo, SA_RESTART, sigaltstack, SEGV handler, sigqueue, sigwait, itimer, nanosleep, setjmp), shell fixes (`$?`, `| read`, `usb selftest`)
- every one of the 152 shell commands was run in QEMU by hand (see regression sessions 1-15 for the automated part)

Round 8: host tests `crypto_host_test` (86 vectors from python `cryptography`), `x509_host_test` (85 checks, generated PKI), `tls_host_test`; `tests/qemu_net_tests.py` (needs root, /dev/net/tun, python `cryptography`, libssh; run by qemu_regression.py) attaches the guest to a TAP bridge and talks to real services:
- tls: unknown CA, `-k`, `--cacert`, `certs add`, RSA-PSS and ECDSA servers, self-signed, wrong root, expired leaf, curl
- ssh: exec/exit status/stderr, three host-key types, wrong password, known_hosts, StrictHostKeyChecking, `-R`, interactive shell, scp up/down/-r/700 KB/missing file
- httpd: python http.client (keep-alive, HEAD, Range, listing, redirects, 400/404/405, traversal, 700 KB file); ftpd: python ftplib (per-user permissions, PASV and active mode, anonymous, `-d` jail, 300 KB transfer)
- pkg: remote add/list/remove, update, install with dependency, upgrade from a second remote, sha256 mismatch rejected, missing index, HTTPS refused without CA / accepted after `certs add` / `-k`, `mkindex`

