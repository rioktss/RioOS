# Crash-safe filesystem and basic security

Scope of this round: the root filesystem survives power loss at any point, the kernel has a real random number generator, packages and repositories can be signed, and `ssh` can log in with an Ed25519 key. The scheduler/SMP/libc work from earlier rounds is untouched (`-smp 1` behaviour unchanged).

## 1. Crash-safe filesystem
- **Metadata journal**: every metadata change (inode table, directory entries, bitmap) is one transaction of at most 32 sectors, written to the journal, flushed, committed, then applied in place. Mount replays committed transactions and ignores the half-written one.
- **Copy-on-write data**: file contents always go to *new* extents; the inode switches to them in the same transaction. After a crash a file is either the old or the new version, never a mix.
- **Derived bitmap**: the free-space bitmap is rebuilt from the inode table at mount, so a crash cannot leak or double-allocate sectors. `write_bitmap` only writes sectors that changed.
- **Atomic replace**: `rename(2)` (syscall `SYS_FS_RENAME`, `fs_rename_replace`) replaces an existing file or symlink atomically (the classic write-temp-then-rename pattern). The shell `mv` still refuses to overwrite. Renaming onto a *directory* still moves into it.
- **Recursive remove**: `rm -r` uses one transaction per node, so a huge tree can no longer overflow the journal; an interrupted `rm -r` leaves a consistent (partially deleted) tree.
- **Auto-repair at mount**: if the structure is inconsistent the kernel runs `fsck_run(repair=1)`: orphan inodes, bad/duplicate names, bad or overlapping extents and bad modes are fixed or dropped. `fsck [-r] [mountpoint]` runs the same checks by hand (`fsck: clean` when nothing is wrong).
- **Crash test** (`tests/fs_crash_test.cpp`, run by `linux/build.sh`; stride 1 for the exhaustive run: `fs_crash_test 1`): simulates power loss after every sector write of a mixed workload, on a write-through disk *and* on a volatile-cache disk where a random subset of unflushed writes is lost. 6185 crash points, 0 failures; plus corruption injection (9 repaired cases).

## 2. Random numbers (`rng`)
SHA-256 hash DRBG (`kernel/random.cpp`) fed by an event pool (timer jitter, UART, network and block I/O timing), virtio-rng (host entropy, `-device virtio-rng-device`, already in all run scripts) and a seed file `/var/lib/random.seed` that is read at boot and refreshed at shutdown or with `rng seed` (root). The generator ratchets after each request. It reports *ready* at ≥128 estimated bits; key generation (`ssh-keygen`, `pkg keygen`) refuses to run before that. `rng` shows the state. Userspace: `getrandom()/getentropy()/arc4random()` (and TLS) draw from it; `rng_ready()` reports readiness.

## 3. Package signatures (`pkg`)
- Ed25519 (RFC 8032, tested with its vectors and against python `cryptography`). Kernel syscall `SYS_ED25519` (112) for userspace.
- `pkg keygen <name>` creates `/etc/pkg/keys/<name>.sec` (mode 0600) and `.pub`. `pkg trust add <file.pub | 64-hex name>`, `trust list`, `trust remove <name>` manage `/etc/pkg/trusted/`.
- `pkg mkindex -s <key> <dir>` writes `INDEX` with a first line `# serial <n>` and `INDEX.sig`. `pkg update` verifies the signature against the trusted keys and refuses a lower serial than the last one seen for that URL (rollback protection). Packages are pinned by the sha256 in the signed index.
- A remote without a valid signed index is refused unless added with `pkg remote add <url> --unsigned`.
- Local files: `pkg sign <key> file.rpk` writes `file.rpk.sig`; `pkg install` verifies it when present; `echo require-signatures > /etc/pkg/policy` makes it mandatory (`-f` overrides). `pkg checksig <file>` checks by hand.

## 4. SSH keys
- `ssh-keygen [-f file] [-C comment]` creates an Ed25519 pair in the OpenSSH `openssh-key-v1` format (default `~/.ssh/id_ed25519`, `/root/.ssh` for root; directory 0700, private key 0600, public 0644; never overwrites). `-y -f file` prints the public key, `-l -f file` the SHA256 fingerprint. Keys are interoperable with OpenSSH/libssh in both directions.
- `ssh` and `scp` try `~/.ssh/id_ed25519` automatically (or `-i file`) and fall back to the password prompt if the server refuses the key. A key file readable by group/other is refused ("UNPROTECTED PRIVATE KEY FILE"). Key material is zeroed after use.
- Install the public key on a server: `scp ~/.ssh/id_ed25519.pub user@host:.ssh/authorized_keys` (or append it there).

## 5. Honest limits
- A journal transaction is limited to 32 sectors: an operation touching more metadata than that (e.g. a file with a very large number of hard links) can fail with an error instead of being applied. `rm -r` is atomic per node, not as a whole. No data journaling beyond copy-on-write of whole files.
- Ed25519 signing is not constant-time (side channels matter only for a shared/hostile machine); private keys are unencrypted (no passphrase support, `-N ""` only); only Ed25519 client keys; there is no sshd and no `authorized_keys` handling on the RioOS side.
- Serial numbers for anti-rollback are tracked per URL; a deleted state file resets the protection. Entropy quality depends on virtio-rng or a seed file from an earlier boot; without both, the estimate climbs slowly.
- Rename onto a directory is "move into"; the shell `mv` never overwrites.

## 6. Tests
`tests/fs_crash_test.cpp`, `tests/fs_host_test.cpp`, `tests/crypto_host_test.cpp` (Ed25519 sign/verify), `tests/sshkey_host_test.cpp` (key format vs libssh), `tests/qemu_net_tests.py` sessions `pkg` (signing, trust, rollback, policy) and `ssh` (keygen, public-key login, fallback, `-i`, permissions), `tests/qemu_regression.py` session 16 (rng, seed across boots, fsck after operations, ssh-keygen).
