# RioOS Development Guide

This guide describes a development workflow that keeps RioOS understandable and reduces the chance of breaking unrelated subsystems.

## Development principles

### 1. Make small, traceable changes

A kernel bug can cross multiple layers. Prefer small commits such as:

```text
fix: validate VFS path before recursive remove
feat: add host regression test for scheduler policy

docs: explain ELF userspace flow
```

### 2. Keep the source tree intact

RioOS is intentionally split into subsystems. Avoid moving or deleting source files unless the change itself requires a structural refactor.

### 3. Test at the lowest useful layer

If a bug can be reproduced by a host-side test, write or update that test before debugging QEMU runtime behaviour.

### 4. Document architectural assumptions

Examples:

- expected CPU/target
- exception-level assumptions
- QEMU device assumptions
- filesystem on-disk expectations
- userspace ABI assumptions

## Build pipeline

`build.sh` currently follows this sequence:

```text
userspace
   ↓
host tools
   ↓
storage image
   ↓
boot assembly
   ↓
kernel compilation
   ↓
linking
   ↓
ELF verification
   ↓
host tests
   ↓
release verification
```

A successful source change should preserve this pipeline.

## Host tests

The repository contains focused host-side tests for several areas:

```text
tests/fs_host_test.cpp
tests/vfs_host_test.cpp
tests/net_host_test.cpp
tests/mem_host_test.cpp
tests/syscall_host_test.cpp
tests/diag_host_test.cpp
tests/sched_host_test.cpp
```

These tests do not replace QEMU testing. They make fast subsystem-level regression checks possible.

## Release verification

`tests/verify_release.py` performs static/release checks around the kernel image and release artifacts.

The repository's test matrix documents which checks are automated and which still need an actual Termux/QEMU or other runtime host.

See [`../tests/TEST_MATRIX.md`](../tests/TEST_MATRIX.md).

## Debugging workflow

When a new feature breaks boot:

```text
1. Reproduce from a clean build
2. Capture the complete serial log
3. Decide whether the failure is build-time or runtime
4. Narrow it to boot / memory / interrupt / driver / process / userspace
5. Add diagnostics or a regression test
6. Fix the smallest layer that owns the bug
7. Re-run host tests
8. Re-run QEMU runtime checks
```

Avoid changing multiple unrelated kernel subsystems at the same time. Bare-metal bugs can otherwise become difficult to localize.

## Safe documentation-first contributions

People who are new to OS development can contribute without changing kernel behaviour:

- README improvements
- architecture diagrams
- setup instructions
- troubleshooting notes
- test documentation
- explanations of source files

These are real engineering contributions and make the repository more useful globally.

## Coding expectations

The existing build treats compiler warnings seriously and uses strict freestanding flags for the kernel. Preserve the current warning discipline and target settings unless the change explicitly requires otherwise.

For kernel C++ code, remember that normal desktop assumptions do not apply. The kernel is built without a host C++ runtime and with freestanding restrictions.

## Pull requests

A useful pull request should explain:

```text
Problem
------
What was wrong?

Change
------
What was changed?

Validation
----------
Which host tests passed?
Did QEMU boot?
What runtime checks were performed?

Compatibility
-------------
Does the change affect the on-disk format, userspace ABI, or QEMU assumptions?
```

## Documentation rule

When a feature changes user-visible behaviour, update the relevant documentation in the same change.

The goal is simple: **the repository should explain how it works, not only contain code that works.**
