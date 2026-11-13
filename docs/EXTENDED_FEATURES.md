# RioOS extended features

All additions keep the existing commands, syscalls, drivers, FS layout and build configs.

## Multitasking and processes
- Each EL0 program is a kernel thread with its own 32 KiB kernel stack and private TTBR0 address space; the 100 Hz timer IRQ preempts EL0 code (`thread_user_preempt`). Kernel mode is non-preemptible.
- Syscalls: FORK 37, EXEC 38, WAITPID 39, GETPPID 40, KILL 41, YIELD 42, BRK 43, MMAP 44, MUNMAP 45, SPAWN 46, PROCINFO 47, SPAWN_FD 49, MPROTECT 50, GETUID 51, SETUID 52, TCP_LISTEN 53, TCP_ACCEPT 54.
- Signals: pending bitmask, default actions at return to EL0 (KILL/TERM/INT terminate, STOP/TSTP stop, CONT resumes, CHLD ignored), SIGPIPE on broken pipes, SIGSEGV on bad access.
- Shell: `cmd &`, `jobs`, `fg`, `bg`, `kill [-SIG] pid|%n`, `wait`, ELF pipelines `a | b | c`, Ctrl-C / Ctrl-Z.

## Memory
- `kmalloc`/`kfree`/`kcalloc`/`krealloc` (first-fit, coalescing); refcounted page allocator.
- Demand paging (lazy brk/mmap/stack), copy-on-write fork, `mmap`/`munmap`/`mprotect` with W^X.

## libc and dynamic linker
- `userspace/libc` (string, stdio-style printf, malloc, POSIX-like wrappers, crt0). `dlopen/dlsym/dlclose` for AArch64 ET_DYN objects (`userspace/so`).
- Example programs in `userspace/bin` (`hello`, `upper`, `lcount`, `spin`, `ktest`, `dltest`).

## Filesystem
- Metadata write-ahead journal (last 33 sectors of the disk, ordered mode); replayed at mount. `jtest` simulates power loss after commit.
- Symbolic links (`ln -s`, `readlink`, followed by path resolution, loop limit 8). `mount -o remount,ro|rw`, `sync`, `umount` (root is busy).
- Journal is disabled automatically on old images whose data overlaps the journal area.

## Security
- `/etc/passwd` (root-only, 0600): `name:uid:gid:$rio$3000$salt$PBKDF2-HMAC-SHA256`. Default accounts root/user (password `123`: change it with `passwd`).
- `su [name]`, `useradd`, `userdel`, `passwd [name]`, `id`, `users`; processes inherit uid/gid and file access uses the process identity; `setuid` only for root.

## Shell scripting
`sh file [args]`, `source`, `.`: `NAME=value`, `$VAR ${VAR} $? $# $0-$9`, `if/elif/else/fi`, `for/while/do/done`, `break/continue/exit/shift`, `test`/`[ ]` (`-f -d -e -L -r -w -x -z -n = != -eq -ne -lt -le -gt -ge !`), `let NAME=expr`, `true/false`. `$?` is the exit status of ELF programs; built-in commands report 0.

## Shell additions
- Exit status for built-ins (a command that prints `name: ...` or `Unknown command` sets `$?` to 1/127), `a && b`, `a || b`, `a; b`.
- Pipelines whose first stage is any built-in (`ls /bin | sort | tail -n 2`); filters: `grep wc head tail sort uniq cut tr tee`; also `head/tail/sort/uniq/cut FILE`.
- `seq`, `sleep`, `rmdir`, `free`, `which`, `basename`, `dirname`, `read`, `poweroff`/`halt`/`reboot` (PSCI).

## Drivers
- PCIe ECAM enumeration with BAR assignment (`lspci`), NVMe (`nvme`), AHCI/SATA (`sata`). With no virtio-blk disk the root filesystem mounts from NVMe, then AHCI.

## Networking
- DHCP client (`dhcp`), TCP server (listen/accept syscalls, `httpd`), IPv6 link-local with NDP and ICMPv6 echo (`ping6`).
- Fixed: virtio-net header is 12 bytes with VERSION_1 (frames were shifted by two bytes).

## SMP
- Secondary cores are released with PSCI CPU_ON (`-smp N`). Since round 9 they schedule user processes in parallel under a Big Kernel Lock; see `docs/IPC_POSIX.md`. Offloaded jobs (`smp`, `smp test`) still work.

## Round 2 additions
- **Shell**: redirects for every built-in (`> >> 2> 2>> &> 2>&1`, `/dev/null`; `<` keeps the legacy `cmd < file`), wildcards `* ?` (`ls *.txt`; `ls cat wc rm stat head tail` run once per match), command substitution `$(cmd)` and backticks (nested, 6 levels), shell functions (`name() { ...; }`, `function name { ...; }`, `return n`, arguments `$1..$9`), `for` lists up to 256 words, tab completion knows all commands.
- **`free`**: heap "used" no longer counts the 32 MB page pool.
- **Filesystem v2**: 256 inodes (was 64) and files up to 4 MiB (was 1 MiB). New disks are formatted v2; v1 images still mount unchanged (layout read from the superblock). `truncate`, `cp` and the truncate syscall use the 4 MiB limit.
- **/tmp**: created if missing (mode 0777) and emptied at every boot ("volatile"; the data lives on the same disk, it is not RAM-backed).
- **Network**: DHCP runs automatically at boot (1.6 s timeout, static 10.0.2.15 otherwise); `dhcp status|renew|release`; the lease is renewed automatically at 50% of its time when the shell shows a prompt; `nc [-z] host port [text]`, `nc -l port`, `telnet host [port]` (Ctrl-] quits), `wget url|host[:port] [path] [file]`.
- **HTTPS**: TLS 1.3 client (TLS_AES_128_GCM_SHA256, X25519, HKDF, AES-128-GCM implemented in `net/tls.cpp`, vector-tested in `tests/tls_host_test.cpp`): `httpsget`, `wget https://...`. The server Finished MAC is verified, but the certificate chain is **not** validated (no trust store) and the random source is a weak timer-based hash: encrypted, not authenticated.
- **USB**: xHCI driver (`drivers/usb.cpp`), polling: HID boot keyboard (types into the shell) and HID boot mouse (deltas/buttons counted); `usb`, `usb selftest`. Use `-device qemu-xhci -device usb-kbd -device usb-mouse`.

## Round 3 additions
- **Multi-mount VFS**: `mount <dev> <dir>`, `mount -r <dev> <dir>`, `mount -t tmpfs none <dir>`, `mkfs <dev>`, `umount <dir>`; `mount` lists mounts, `df [-h]` shows every mount. Devices: `ram0`, `ram1` (2 MiB RAM disks), `nvme0n1` and `sda` (NVMe / AHCI when they are not the root disk; QEMU `-device nvme`, `-device ich9-ahci`). Each mount is its own myfs instance (own inode table, bitmap, journal). Global node id = `(mount << 12) | inode`; paths cross mount points and `..` leaves them. `/tmp` is now a real tmpfs (RAM disk 0) mounted at boot. `mv` across mounts copies then removes (files and directories); `rename` itself returns EXDEV. A mount point cannot be removed; `umount` refuses when the shell is inside it or another mount is nested in it. Up to 8 mounts (including root). Mounts are not remembered across reboots (no fstab).
- **IPv6 UDP/TCP** (link-local, NDP-resolved): `udp6 send|listen`, `nc`/`telnet` accept IPv6 literals, `nc -l` accepts IPv4 or IPv6. Pseudo-header checksum verified in both directions.

## Round 4 additions
- **Help**: `help` now shows categories (files, text, shell, system, users, storage, network, libc); `help <category>` and `help all` list the commands; `man <command>` for details. Every command is listed.
- **Text tools** (`kernel/textutils.cpp`, own regex engine): `awk` (integer arithmetic, no FPU), `sed` (s d p q y =, addresses), `xargs`, `rev`, `tac`, `nl`, `more`/`less`. Work in pipes and on files.
- **Shell**: `alias`/`unalias`, `printf`, `expr`, quote-aware redirects, persistent history (`history -w` saves to `/home/user/.history`, loaded at boot, `history -r` reloads).
- **Filesystem**: hard links (`ln a b`, shared data, copy-on-write on write, link count in `stat`), `fsck [-r]`, per-user `quota` (`/etc/quota`, enforced on writes, root exempt), **FAT32** read/write with long names through the `fat` command on a raw device (not VFS-mounted).
- **Network**: `curl` (GET/POST/HEAD, -H, -d, -L, -o, -i/-I), `ntpdate [-q]` (SNTP), `fw` (IPv4 packet filter, ordered rules, default policies).
- **User libc** (`userspace/libc/ulibc2.cpp`): stdio (printf family, fopen/fgets/fputs...), string and integer math functions, `libctest` (18 checks), `hexdump` program.

## Round 5 additions (libc, archives, shell, compiler, editor, packages)
- **User libc** (`userspace/libc/`): IEEE-754 soft-float (`softfloat.cpp`, bit-exact + - * / sqrt, conversions; the CPU is used with `-mgeneral-regs-only`, doubles are passed in integer registers), `ulibm.cpp` (exp log pow sin cos tan asin acos atan atan2 sinh cosh tanh floor ceil round fmod ldexp frexp cbrt hypot fma, float variants), printf `%f %e %g` (use `va_arg_double`, never `va_arg(ap,double)`), `strtod/atof/strtof`, a full `scanf` family, `getline`, `setlocale/localeconv` (C locale only), ctype extras, `time/gmtime/localtime/mktime/strftime` (UTC only), `getenv/setenv`, improved `malloc/realloc/aligned_alloc`, syscalls 55-59 (mkdir, unlink, rename, readdir, epoch). `libctest` now runs 54 checks. Host tests (`softfloat_host_test`, `libm_host_test`, `deflate_host_test`) run from `linux/build.sh`.
- **Archives**: `gzip/gunzip/zcat` (own DEFLATE: stored/fixed/dynamic Huffman, hash chains, lazy matching, CRC32; levels 1-9; interoperable with zlib) and `tar` (ustar, `-c -x -t -v -z -C -f`, long names via prefix, unsafe `..` paths refused). Cross-checked in both directions with host Python `gzip`/`tarfile`.
- **diff** (`-u -U N -q -i -w -b -r -s`, exit codes 0/1/2; LCS with prefix/suffix trimming).
- **Shell**: `case ... esac` (patterns, `|`, `;;`), `until`, quote handling for `echo`, `cat` as a pipe filter, context-aware Tab completion for arguments (files, directories, `$VAR`, `fat` devices/subcommands).
- **vi**: modal editor (normal/insert/replace/ex), counts, `. u ^R`, search, `:s`, `:r`, `:set nu`, `-R`.
- **pkg**: package manager for `.rpk` files (tar + `PKGINFO`): install/remove/upgrade/search/verify/owner/build/update, dependency resolution from the local repository `/var/cache/pkg` (with `>= > <= < =` version constraints), conflicts/provides, file-ownership checks, upgrades drop stale files, database `/var/lib/pkg`.
- **cc**: C-subset compiler to bytecode plus a bounds-checked VM (`cc file.c`, `cc -S`, `cc -c`, `cc -o img`, `cc -x img`): int/long/char/unsigned, pointers, arrays, enum, typedef, `#define` (object and function-like), `#ifdef`, recursion, switch, initializers, printf/sprintf/snprintf/scanf, malloc/free, string functions.
- **Limitations (honest list)**: `cc` generates bytecode, not native AArch64 code, and runs at roughly 1-2 million VM instructions per second under QEMU TCG; no float/double, struct/union, goto or `#if` expressions; `free()` only reclaims the most recent block. `pkg` has no network fetch (local repository only). `vi` assumes an 80x24 terminal and keeps 60 whole-buffer undo snapshots. `localtime` is UTC, only the C locale exists, no `long double`; `strtod` outside the exact range is within 1-2 ulp. `diff` limits LCS to 6 million cells. `gzip` has no `-r`. `tar` cannot use `-C` when creating.

## Round 6: full symlinks and a POSIX process layer
- **Symlinks on every filesystem**: the VFS router expands symlinks across mount points (absolute targets restart at the global root, relative ones resolve from the link's directory, at most 40 expansions, loops give ELOOP). Verified on the root myfs and on tmpfs / second-disk mounts, and between them. `fs_get_parent` of a mount root returns the parent mount, so `pwd` after `cd` through links is correct. Hard links stay within one filesystem.
- **Userspace API** (syscalls 60-64): `symlink link readlink lstat_path realpath` (+ existing `stat_path` which follows links). Shell: `stat` shows a link as itself (`-L` follows), `find -type l`, `readlink -f`, `realpath`, `cp -P` (links copied as links, also inside `cp -r -P`), `tar` stores and restores symlinks (type `2`) and refuses to extract through a symlinked parent directory.
- **Signals** (syscalls 65-70): `sigaction` (handler, `sa_mask`, `SA_NODEFER`, `SA_RESETHAND`), `signal`, `raise`, `sigprocmask`, `sigpending`, `sigsuspend`, `pause`, `alarm` (SIGALRM), `sigreturn` via a libc trampoline. A caught signal pushes a frame on the user stack; the signal is blocked while its handler runs. `sleep`/`usleep` return early when a signal arrives. `fork` inherits handlers and the mask, `exec` resets caught handlers to default and keeps ignored ones. SIGKILL/SIGSTOP cannot be caught or blocked.
- **exec/POSIX**: `execve execv execvp execl execlp execle` with a real environment (carried after `\x01` in the 1024-byte argument block, argv[0] preserved), `vfork` (= copy-on-write `fork`), `system`, `popen/pclose`, `posix_spawn/posix_spawnp` with file actions, `fdopen`, `fcntl` (`F_DUPFD F_GETFD F_SETFD`, `FD_CLOEXEC`, `O_CLOEXEC`; descriptors are closed on exec), W* status macros, `getpgrp/getpgid/setsid/setpgid` (single session stubs).
- **mmap** (syscalls 72-73): `MAP_SHARED|MAP_ANONYMOUS` memory shared across `fork`; `MAP_SHARED` on a file backed by a global page cache, so every process mapping the same file page sees the same memory immediately; data is written back to the file on `msync`, `munmap`, `exit` and `exec`. A writable shared mapping needs an `O_RDWR` descriptor (EACCES otherwise). `MAP_PRIVATE` behaves as before.
- **Limitations (honest list)**: `MAP_FIXED` is rejected (EINVAL) and the address hint is ignored; `read()`/`write()` on a file are not coherent with a live shared mapping until `msync`/`munmap`; file mappings are limited to 4 MiB and files to 1 MiB; a handler for SIGSEGV is not supported (the process is terminated); `SA_RESTART` is accepted but blocking calls return EINTR; no `siginfo`/`sigaltstack`/real-time signals; `system()`/`popen()` use a small built-in interpreter (`; && || | < > >> &`, quotes, `$VAR`, builtins `echo exit true false export unset`) and can only start ELF programs, not kernel-shell built-ins such as `ls`; no process groups or controlling terminals; the `fat` tool (raw FAT32 images) has no symlinks, and the 100-byte tar link-name limit applies to `tar`.

## Round 7: pthread and complete userspace signals
- **Signals** (syscalls 65-77): `sigaction` with `SA_SIGINFO`, `SA_RESTART`, `SA_NODEFER`, `SA_RESETHAND`, `SA_ONSTACK`; handlers get `siginfo_t` (signo, code, sender pid, value, fault address) and a `ucontext`; `sigaltstack`; `sigqueue` (value), `sigwait/sigwaitinfo/sigtimedwait`, `sigsuspend`, `sigpending`, `alarm`, `setitimer/getitimer` (ITIMER_REAL, repeating), `nanosleep` (returns remaining time on EINTR), `clock_gettime`, `gettimeofday`, `strsignal`, `psignal`, `sigsetjmp/siglongjmp/setjmp/longjmp`. SIGSEGV/SIGBUS/SIGILL are delivered to a user handler (with the fault address); a fault inside that handler terminates the process. `SA_RESTART` makes libc `read`/`write` retry. SIGKILL/SIGSTOP cannot be caught or blocked. `exec` resets caught handlers and keeps ignored ones.
- **pthread** (`userspace/libc/ulibc5.cpp`): `pthread_create/join/detach/exit/self/equal/cancel/kill/once/yield`, attributes (stack size, detach state), mutex (normal/recursive/errorcheck, trylock, timedlock), condition variables (wait/timedwait/signal/broadcast), rwlock, barrier, spinlock, keys with destructors, `sem_*`, cancellation points, thread-directed signals, deadlock detection (exit 134 with a message). Up to 32 threads, mmap'd stacks (default 64 KiB).
- **Shell fixes found while testing every command**: `$?` after `cmd; echo $?` in scripts, `cmd | read VAR`, `usb selftest` without a controller, `exec NAME`, `tree PATH`, `grep -v` and `cat | nl` trailing empty line.
- **Limitations (honest list)**: threads are user-level, cooperative and M:1 (one CPU thread of execution, no preemption: a thread that never yields or blocks in the library starves the rest); a blocking kernel call (e.g. reading a pipe, `wait`) blocks all threads; no `__thread` TLS and no guard pages (stack overflow is not detected); no real-time signals queue (one pending value per signal); signals are process-wide, the thread-directed ones (`pthread_kill`) are emulated inside libc; `MAP_FIXED` is still rejected.

## Round 8: certificate verification, ssh/scp, servers, pkg over the network
- **TLS 1.3 certificate verification** (`net/crypto.cpp`, `net/x509.cpp`, `net/trust.cpp`, `net/tls.cpp`): from-scratch SHA-384/512, RSA PKCS#1 v1.5 and PSS (SHA-256/384/512), ECDSA P-256/P-384, Ed25519 and a DER X.509 parser. The client checks the Certificate chain (signatures, validity dates from the RTC, basicConstraints/CA, keyUsage, pathLen, name constraints are not evaluated), the CertificateVerify over the transcript hash and the host name (SAN dNSName/iPAddress, wildcard on one label, CN only when no SAN). Trust store: `/etc/ssl/certs.pem` (117 CA certificates from the build host, baked into the image) plus `certs add <file>`; `--cacert <file>` for one run, `-k/--insecure` to skip (httpsget, wget, curl). `certs` lists/adds/verifies.
- **ssh / scp client** (`net/ssh.cpp`, commands): SSH-2 with curve25519-sha256, host keys ssh-ed25519 / rsa-sha2-256/512 / ecdsa-sha2-nistp256 (signature verified), aes128-ctr + hmac-sha2-256, password authentication, exec and interactive shell (`-t`, escape `~.`), exit status, `scp [-r] [-P port]` both directions. Host key fingerprints are kept in `/etc/ssh_known_hosts` (trust on first use, a changed key is refused, `-o StrictHostKeyChecking=yes|no`, `ssh -R host` forgets one). Tested against libssh servers (ed25519, RSA, ECDSA keys), 700 KB files compared bit for bit.
- **httpd** (`httpd [port] [max-requests] [-d dir] [-q]`): HTTP/1.0 and 1.1 with keep-alive, GET/HEAD, Range (206/416), directory listings, 301 to the trailing slash, 404/405/400, `..` and `%2e%2e` rejected, files served with the permissions of the logged-in user. **ftpd** (`ftpd [-p port] [-n sessions] [-a] [-d root]`): USER/PASS against the system password file (per-user file permissions), anonymous read-only with `-a`, PASV/EPSV/PORT, LIST/NLST/RETR/STOR/APPE/DELE/MKD/RMD/RNFR/RNTO/SIZE/MDTM/CWD/CDUP/PWD/TYPE/REST, root confinement with `-d`.
- **pkg over the network**: `pkg remote add|remove|list`, `pkg update` fetches each remote `INDEX`, `pkg install/upgrade/search` use the best version across the local cache and the remotes; every download is verified against the sha256 in the index (mismatch = rejected, nothing installed), HTTPS remotes are certificate-verified (`-k` / `--cacert` per remote); `pkg mkindex <dir>` produces the INDEX. Kernel support: syscalls 78 (`http_get`) and 79 (`sha256`).
- **Fixes found on the way**: TCP passive open sent no ARP request when the peer was unknown (inverted condition), TCP ephemeral ports restarted at 49152 after every boot (clashes with TIME_WAIT on the peer), `pkg` version comparison could loop forever on a trailing non-numeric part.
- **Limitations (honest list)**: TLS 1.3 only, no client certificates, no revocation checks (CRL/OCSP), no name-constraint or policy evaluation, no session resumption; ssh is client only (no server), password authentication only (no public keys, agent or port forwarding), no compression; httpd/ftpd are plain-text (no HTTPS/FTPS), serve one connection at a time, no CGI, no uploads in httpd; pkg packages are limited to 1 MB per download and index signatures are not supported (integrity comes from the sha256 in an index fetched over HTTPS; over plain HTTP an attacker who controls the index controls the packages).

### Round 8 additions (follow-up)
- **DNS fallback**: if the configured DNS server gives no usable answer (no reply, SERVFAIL, REFUSED - typical when the emulator's DNS proxy has no host resolver, e.g. Termux), the resolver retries with 8.8.8.8 and then 1.1.1.1. NXDOMAIN is final. Needs a working route to the internet; `dnsserver <ip>` still sets the primary server.
- **ftpd `-P lo-hi`**: fixed passive data port range (default 30000-31999) so an emulator can forward the data ports.
- **`RIOOS_FWD`** (run scripts for Linux, Termux, macOS): comma separated hostfwd rules for host->guest servers, e.g. `RIOOS_FWD="tcp::8080-:8080,tcp::2121-:21,tcp::30000-:30000" bash termux/run-terminal.sh`, then inside RioOS `httpd 8080` / `ftpd -P 30000` and from the host `curl http://127.0.0.1:8080/` or `curl ftp://root:123@127.0.0.1:2121/`. `linux/run.sh` keeps its old default (8080 and 2121); the others forward nothing unless asked.

## Round 9: crash-safe filesystem and basic security
See `docs/SECURITY_FS.md`: metadata journal + COW + derived bitmap, atomic `rename(2)` replace, per-node `rm -r`, mount-time auto-repair and `fsck`, exhaustive crash-injection test; CSPRNG (`rng`, virtio-rng, seed file); Ed25519 and signed pkg repositories (trust store, anti-rollback, policy); `ssh-keygen` and public-key login for `ssh`/`scp`.

## Not implemented
Parallel kernel execution (SMP uses a Big Kernel Lock; only user code runs in parallel), mounts persisted in an fstab, IPv6 global addressing / routers / DNS / DHCPv6, TLS certificate validation and TLS 1.2, HTTPS server, sshd, ext2, atime/ctime and disk encryption, USB hubs/storage/tablet devices, Wi-Fi and other NICs, audio/GPU, booting on real hardware (Raspberry Pi), native machine-code compilation, kernel floating-point (userspace floats are soft-float), job-control signals beyond what `jobs/fg/bg/kill` already do.
