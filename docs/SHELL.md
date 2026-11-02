# RioOS shell guide (v1.14.0)

The RioOS shell is a small built-in shell, not GNU Bash. Version 1.14.0 turns
it from a demo prompt into something you can actually script and pipe with.

## Chaining and history

| Syntax | Meaning |
|---|---|
| `cmd1; cmd2; cmd3` | Run commands in order. A `;` inside single or double quotes is literal. |
| `!!` | Repeat the previous command (it is echoed first). |
| `!N` | Repeat history entry N (1-based, see `history`). |
| Tab | Completes every built-in command name and file/directory names. |

Because `;` is now a separator, `echo a;b` runs `b` as a command. Quote it
(`echo "a;b"`) to print the semicolon.

## Text tools

All of these work on a file (`sort names.txt`) and as pipeline stages.

| Command | Purpose |
|---|---|
| `head [-n N]` / `tail [-n N]` | First / last N lines (default 10). `-5` also works. |
| `sort [-r] [-n] [-u]` | Bytewise sort; `-n` numeric, `-r` reverse, `-u` unique. Stable. |
| `uniq [-c] [-d]` | Collapse adjacent duplicates; `-c` counts; `-d` only repeated lines. |
| `rev` / `tac` / `nl` | Reverse characters / reverse line order / number lines. |
| `cut -d C -f LIST` / `cut -c LIST` | Fields or columns. LIST is `N`, `N-M`, `N-`, `-M`, comma separated. |
| `tr [-d] SET1 [SET2]` | Translate or delete characters; ranges such as `a-z` are allowed. |
| `grep`, `wc` | Also work as later pipeline stages. |

Examples:

```text
cat words.txt | sort | uniq -c | sort -nr | head -n 5
tr a-z A-Z notes.txt | head -n 3
echo a:b:c | cut -d : -f 2
cat log.txt | grep error | sort -u > errors.txt
```

A pipeline may start with `cat <file>`, `echo <text>`, or a file filter such
as `tail log.txt`. The last stage may end with `> file` or `>> file`.

Limits: 4096 lines and 256 KiB per stage. Going over is reported as an error,
never silently truncated.

## Small utilities

| Command | Purpose |
|---|---|
| `expr (2+3)*4` | 64-bit integer math with `+ - * / %`, parentheses, unary minus. |
| `seq [first [step]] last` | Number sequence (max 100000 numbers). |
| `basename <path>` / `dirname <path>` | Split a path. |
| `sleep <0-60>` | Wait, using the timer IRQ instead of spinning. |
| `which <name>` | Reports alias, shell built-in, or not found. |

## Aliases and scripts

```text
alias ll=ls -l          # define (16 max)
alias                   # list
unalias ll              # remove one; unalias -a removes all
source script.sh        # run a file, one command per line ('.' is a synonym)
```

Scripts skip blank lines and lines starting with `#`, and can nest up to 3
levels. At startup the shell silently runs `/home/user/.riorc` if it exists,
so aliases can persist across reboots:

```text
echo "alias ll=ls -l" > .riorc
```

Aliases are expanded once at the start of a command, so they cannot loop.

## Implementation notes

* `kernel/textutil.cpp` holds the filters, the expression evaluator and the
  path helpers. It has no heap and no libc, and is covered by
  `tests/textutil_host_test.cpp` (run by every build script).
* `kernel/commands.cpp` wires the filters into the shell and the pipeline.
* Not implemented: `&&`/`||`, `$?`, quoting inside pipeline arguments, shell
  functions, job control.
