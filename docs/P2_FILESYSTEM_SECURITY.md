# P2: Filesystem and permission hardening — RioOS 1.8.0

## What changed

- Each filesystem node now carries persisted `uid` and `gid` fields in memory. On disk these fields and a metadata marker use the previously reserved bytes of the existing 128-byte inode, so the superblock version and inode size remain unchanged.
- Old inodes with no marker are migrated after structural validation. The root inode and entries outside user homes become root-owned; entries below `/home/user` or `/storage/home` become UID/GID 1000. Migration is written only after the mounted tree passes structural checks.
- Permission checks follow owner, group, then other mode bits. UID 0 bypasses normal data-access checks. Path traversal requires execute/search permission; file read/write, directory mutation, rename/remove, `chmod`, and `chown` enforce access rules. ELF loading additionally requires at least one execute bit, including when launched from a root shell.
- `chmod` is allowed to the owner or root. `chown` is root-only. New files inherit current credentials. Standard user homes are assigned to UID/GID 1000 during mount initialization.
- `stat`, `chown`, and `truncate` are exposed as shell commands. Truncate accepts 0 through 262144 bytes, preserves existing bytes and zero-fills extensions.
- ELF loading requires execute permission. Raw `diskread`, `diskwrite`, and `diskflush` commands require root so the shell cannot trivially bypass filesystem checks by editing raw sectors.
- Added `SYS_FS_STAT`, `SYS_FS_TRUNCATE`, and `SYS_FS_CHMOD` (numbers 23–25) and `user_fs_stat`, `user_fs_truncate`, `user_fs_chmod` helpers. The syscall path temporarily uses unprivileged UID/GID 1000 for EL0 calls, then restores the prior EL1 credentials.

## Quick shell test

```text
touch /storage/home/demo.txt
write /storage/home/demo.txt hello
stat /storage/home/demo.txt
chmod 600 /storage/home/demo.txt
truncate /storage/home/demo.txt 8
cat /storage/home/demo.txt
su root
# enter the development password: rioos-root
chown 0 0 /storage/home/demo.txt
logout
```

To verify denial, create a root-owned file, set mode `600`, return to the user identity, then try `cat`, `write`, or `chmod` on that file. User access should be denied.

## Limitations

This remains an educational single-user-credential model: root UID/GID are kept in a shell-global role, with EL0 syscalls forced to UID/GID 1000. Credentials are not stored per process, so this is not a hardened isolation boundary for concurrent processes. The development root password is embedded in source and must be changed for local experiments; it is not production-grade authentication. There is no journal, symlink support, advanced ACL, sticky bit, or crash-atomic multi-inode transaction layer. Timer IRQ is not enabled by this increment.

## Tests

`tests/fs_host_test.cpp` exercises denied reads/writes and `chmod`, owner-level `chmod`, root-only owner changes, stat contents, truncate/zero-fill/maximum-size error, metadata persistence, and legacy inode metadata migration. `tests/syscall_host_test.cpp` covers new syscall dispatch and confirms EL0 filesystem credential overrides are restored after returning to EL1. `tests/verify_release.py` checks the P2 source/ABI invariants.
