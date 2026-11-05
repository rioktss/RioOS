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
- Secondary cores are released with PSCI CPU_ON (`-smp N`), run on the kernel page tables and execute offloaded jobs (`smp`, `smp test`). The scheduler, heap and drivers remain on CPU0.

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
- **Limitations (honest list)**: `cc` generates bytecode, not native AArch64 code, and runs at roughly 1-2 million VM instructions per second under QEMU TCG; no float/double, struct/union, goto or `#if` expressions; `free()` only reclaims the most recent block. `pkg` has no network fetch (local repository only). `vi` assumes an 80x24 terminal and keeps 60 whole-buffer undo snapshots. `localtime` is UTC, only the C locale exists, no `long double`; `strtod` outside the exact range is within 1-2 ulp. `diff` limits LCS to 6 million cells. `gzip` has no `-r`. `tar` skips symlinks and cannot use `-C` when creating.

## Not implemented
Scheduler running EL0 processes on several CPUs (secondaries still only run offloaded jobs; this needs a kernel-wide lock and per-CPU state, so it was left out rather than risk corrupting the single-CPU kernel), mounts persisted in an fstab, IPv6 global addressing / routers / DNS / DHCPv6, TLS certificate validation and TLS 1.2, HTTPS server, SSH, ext2, atime/ctime and disk encryption, USB hubs/storage/tablet devices, Wi-Fi and other NICs, audio/GPU, booting on real hardware (Raspberry Pi), native machine-code compilation, kernel floating-point (userspace floats are soft-float), job-control signals beyond what `jobs/fg/bg/kill` already do.
