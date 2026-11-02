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

## Drivers
- PCIe ECAM enumeration with BAR assignment (`lspci`), NVMe (`nvme`), AHCI/SATA (`sata`). With no virtio-blk disk the root filesystem mounts from NVMe, then AHCI.

## Networking
- DHCP client (`dhcp`), TCP server (listen/accept syscalls, `httpd`), IPv6 link-local with NDP and ICMPv6 echo (`ping6`).
- Fixed: virtio-net header is 12 bytes with VERSION_1 (frames were shifted by two bytes).

## SMP
- Secondary cores are released with PSCI CPU_ON (`-smp N`), run on the kernel page tables and execute offloaded jobs (`smp`, `smp test`). The scheduler, heap and drivers remain on CPU0.

## Not implemented
HTTPS/TLS, USB/HID, scheduler running on several CPUs, multi-mount VFS (single root mount), DHCP lease renewal, IPv6 global addressing/TCP/UDP.
