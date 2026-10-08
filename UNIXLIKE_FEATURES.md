# RioOS / MyKernel Unix-like shell features

This update is additive: existing kernel, filesystem, ELF userspace, VirtIO, scheduler, security, and diagnostic commands remain in place. Timer IRQs are still **OFF by default** and are not enabled by normal shell startup or by the new shell features.

## Text search and file discovery

- `grep [options] <pattern> <file>`
  - `-n` show line numbers
  - `-i` ignore case
  - `-v` invert match
  - `-c` count matching lines only
- `find [path] -name <name>`
- `find [path] -type <f|d>`

Examples:

```text
find / -name file.txt
find /home -type d
grep -n halo file.txt
grep -iv halo file.txt
grep -c halo file.txt
```

Matching is substring based rather than regular-expression based.

## Command history and editing

- `history` show all retained history
- `history 10` show the last 10 commands
- `history -c` clear history
- Arrow Up / Down recall previous commands
- `Tab` completes command names and filesystem entries

History is kept in RAM for the current shell session (32 entries).

## Date and time

- `date` show RTC date/time
- `date -u` show RTC time in UTC mode
- `date +%Y-%m-%d` formatted output
- `date -s YYYY-MM-DD HH:MM:SS` set the QEMU virt PL031 RTC

The RTC path is polled directly; no timer IRQ is required.

## Storage usage

- `du [options] [path]`
  - `-h` human-readable units
  - `-s` summary only
- `df [options]`
  - `-h` human-readable units
- `wc [options] <file>`
  - `-l` lines
  - `-w` words
  - `-c` characters/bytes

## Permissions

- `chmod 777 file.txt`
- `chmod +x file.elf`
- `chmod -rwx file.txt`

Modes are stored in the persistent inode metadata and survive remounts/reboots. Symbolic `+rwx`/`-rwx` applies the requested permission bits to all three classes.

## Shell variables

- `env` show environment variables
- `export VAR=val` set a variable
- `echo $VAR` expand a variable

Variables are shell-session state and are not persisted to disk.

## Redirection and pipes

- `echo hi > file.txt` overwrite
- `echo hi >> file.txt` append
- `cat < file.txt` input redirection
- `cat file.txt | grep halo` in-memory pipe

For safety in this bare-metal shell, the pipe implementation currently supports `cat`/`echo` as the left side and `grep` as the right side. This avoids introducing a blocking stream/pipe subsystem into the existing shell while keeping the requested example functional.

## IRQ safety

Normal boot still does:

```text
timer_init();        // timer hardware OFF
...
interrupt_init();    // timer PPI masked
...
shell();             // no timer IRQ startup
```

Periodic/one-shot timer IRQ testing remains reachable only through the existing explicit `irq ...` diagnostics.
