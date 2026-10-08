#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

VERBOSE=0
RUN_QEMU=0
CLEAN=0

for arg in "$@"; do
    case "$arg" in
        -v|--verbose) VERBOSE=1 ;;
        --run) RUN_QEMU=1 ;;
        --clean) CLEAN=1 ;;
        -h|--help)
            cat <<'EOF'
RioOS portable build system

Usage:
 ./build.sh Build + tests only
 ./build.sh --run Build + tests + start QEMU
 ./build.sh -v Verbose compiler output
 ./build.sh --clean Remove generated build/host-test outputs
 ./build.sh --clean --run
EOF
            exit 0
            ;;
        *) echo "ERROR: unknown argument: $arg" >&2; exit 2 ;;
    esac
done

log() { printf '%s\n' "$*"; }
vlog() { [[ $VERBOSE -eq 1 ]] && printf '%s\n' "$*" || true; }
step() { printf '[%s] %s\n' "$1" "$2"; }

printf '\n==================================\n'
printf ' RioOS Portable Build v2.0 + nano\n'
printf '==================================\n\n'

CLANG="${CLANG:-clang}"
CXX="${CXX:-clang++}"
LD="${LD:-ld.lld}"
READELF="${READELF:-llvm-readelf}"
PYTHON="${PYTHON:-}"

find_python() {
    if [[ -n "$PYTHON" ]] && command -v "$PYTHON" >/dev/null 2>&1; then
        printf '%s' "$PYTHON"
        return
    fi
    for p in python3 python; do
        if command -v "$p" >/dev/null 2>&1; then
            printf '%s' "$p"
            return
        fi
    done
    return 1
}

PYTHON="$(find_python || true)"

require_cmd() {
    command -v "$1" >/dev/null 2>&1 || {
        echo "ERROR: required command not found: $1" >&2
        echo " Set PATH or override the corresponding environment variable." >&2
        exit 1
    }
}

require_cmd "$CLANG"
require_cmd "$CXX"
require_cmd "$LD"

if [[ -z "$PYTHON" ]]; then
    echo "ERROR: Python 3 is required for tests/verify_release.py" >&2
    exit 1
fi

mkdir -p build storage storage/home
HOST_TOOLS="${MYKERNEL_TOOLS_DIR:-$HOME/.mykernel-tools}"
mkdir -p "$HOST_TOOLS"

if [[ "$CLEAN" -eq 1 ]]; then
    rm -f build/*.o build/kernel.elf build/user_init.elf build/nano.elf
    rm -f "$HOST_TOOLS"/fs_host_test
    rm -f "$HOST_TOOLS"/net_host_test
    rm -f "$HOST_TOOLS"/mem_host_test
    rm -f "$HOST_TOOLS"/syscall_host_test
    rm -f "$HOST_TOOLS"/diag_host_test
fi

# Map module name -> source path.
src_of() {
    local f="$1" d
    for d in kernel drivers memory process fs net; do
        [[ -f "$d/$f.cpp" ]] && { printf '%s' "$d/$f.cpp"; return; }
    done
    echo "ERROR: source for $f not found" >&2
    exit 1
}

CXXFLAGS=(
    -Iinclude
    --target=aarch64-none-elf -mcpu=cortex-a72 -mgeneral-regs-only -mstrict-align
    -ffreestanding -fno-exceptions -fno-rtti -fno-stack-protector -fno-builtin
    -fno-pic -fno-pie -fno-vectorize -fno-slp-vectorize
    -fno-unwind-tables -fno-asynchronous-unwind-tables
    -nostdinc++ -std=c++17 -O1 -Wall -Wextra -Werror
)
ASMFLAGS=(
    -Iinclude
    --target=aarch64-none-elf -mcpu=cortex-a72
    -ffreestanding -fno-pic -fno-pie
)

MODULES=(
    uart memory string fs vfs process scheduler timer interrupt keyboard
    commands shell exceptions syscall user mmu virtio_mmio virtio_blk storage
    fb font
    elf virtio_net netstack page_alloc fault diag kernel
)

step "1/9" "Bundling userspace (init + nano)"
"$CLANG" "${ASMFLAGS[@]}" -c userspace/user_init.S -o build/user_init.o
"$LD" -T userspace/user_init.ld -nostdlib -z max-page-size=0x1000 \
    build/user_init.o -o build/user_init.elf

# --- NANO APP ---
if [[ -f userspace/nano.S ]]; then
    "$CLANG" "${ASMFLAGS[@]}" -c userspace/nano.S -o build/nano.o
    "$LD" -T userspace/user_init.ld -nostdlib -z max-page-size=0x1000 \
        build/nano.o -o build/nano.elf
    vlog " -> build/nano.elf ready"
else
    echo "WARN: userspace/nano.S not found, skipping nano build"
    # bikin dummy biar mkfs gak error
    cp build/user_init.elf build/nano.elf 2>/dev/null || true
fi

step "2/9" "Building host tools"
"$CXX" -std=c++17 -O2 -Wall -Wextra -pedantic \
    fs/mkfs_myfs.cpp -o "$HOST_TOOLS/mkfs_myfs"

step "3/9" "Preparing disk image"
if [[ ! -f storage/disk.img ]]; then
    dd if=/dev/zero of=storage/disk.img bs=512 count=32768 status=none
fi

# MULTI-APP SUPPORT: sekarang kirim 2 elf, mkfs yang baru akan loop
if [[ -f build/nano.elf ]]; then
    "$HOST_TOOLS/mkfs_myfs" storage/disk.img build/user_init.elf build/nano.elf >/dev/null
else
    "$HOST_TOOLS/mkfs_myfs" storage/disk.img build/user_init.elf >/dev/null
fi
vlog " -> storage/disk.img ready (16 MiB) with apps"

step "4/9" "Compiling boot"
"$CLANG" "${ASMFLAGS[@]}" -c boot/boot.S -o build/boot.o
"$CLANG" "${ASMFLAGS[@]}" -c boot/exceptions.S -o build/exception_vectors.o
"$CLANG" "${ASMFLAGS[@]}" -c process/user_entry.S -o build/user_entry.o

step "5/9" "Compiling kernel (${#MODULES[@]} modules)"
for f in "${MODULES[@]}"; do
    if [[ "$VERBOSE" -eq 1 ]]; then
        echo " -> $f.cpp"
        "$CXX" "${CXXFLAGS[@]}" -c "$(src_of "$f")" -o "build/$f.o"
    else
        printf '.'
        if ! "$CXX" "${CXXFLAGS[@]}" -c "$(src_of "$f")" -o "build/$f.o"; then
            echo
            echo "ERROR: gagal compile $f.cpp" >&2
            exit 1
        fi
    fi
done
[[ "$VERBOSE" -eq 0 ]] && printf ' done\n'

step "6/9" "Linking"
"$LD" -T linker.ld -nostdlib -z max-page-size=0x1000 \
    build/boot.o build/exception_vectors.o build/user_entry.o \
    build/kernel.o build/uart.o build/memory.o build/string.o build/fs.o build/vfs.o \
    build/process.o build/scheduler.o build/timer.o build/interrupt.o build/keyboard.o \
    build/commands.o build/shell.o build/exceptions.o build/syscall.o build/user.o build/mmu.o \
    build/virtio_mmio.o build/virtio_blk.o build/storage.o build/elf.o build/virtio_net.o \
    build/netstack.o build/page_alloc.o build/fault.o build/diag.o \
    build/fb.o build/font.o \
    -o build/kernel.elf

step "7/9" "Verifying ELF"
if command -v "$READELF" >/dev/null 2>&1; then
    "$READELF" -h build/kernel.elf | grep -E 'Class:|Machine:|Entry point' || true
else
    vlog " -> $READELF not found, skip"
fi

step "8/9" "Running host tests"
"$CXX" -std=c++17 -O2 -Wall -Wextra -pedantic -iquote include -DHOST_TEST \
    tests/fs_host_test.cpp fs/fs.cpp kernel/string.cpp \
    -o "$HOST_TOOLS/fs_host_test"
"$HOST_TOOLS/fs_host_test"

"$CXX" -std=c++17 -O2 -Wall -Wextra -Werror -pedantic -iquote include -DHOST_TEST \
    tests/net_host_test.cpp net/netstack.cpp \
    -o "$HOST_TOOLS/net_host_test"
"$HOST_TOOLS/net_host_test"

"$CXX" -std=c++17 -O2 -Wall -Wextra -pedantic -iquote include -DHOST_TEST \
    tests/mem_host_test.cpp memory/page_alloc.cpp kernel/fault.cpp \
    -o "$HOST_TOOLS/mem_host_test"
"$HOST_TOOLS/mem_host_test"

"$CXX" -std=c++17 -O2 -Wall -Wextra -pedantic -iquote include -DHOST_TEST \
    tests/syscall_host_test.cpp kernel/syscall.cpp \
    -o "$HOST_TOOLS/syscall_host_test"
"$HOST_TOOLS/syscall_host_test"

"$CXX" -std=c++17 -O2 -Wall -Wextra -pedantic -iquote include -DHOST_TEST \
    tests/diag_host_test.cpp kernel/diag.cpp \
    -o "$HOST_TOOLS/diag_host_test"
"$HOST_TOOLS/diag_host_test"

"$CXX" -std=c++17 -O2 -Wall -Wextra -Werror -pedantic -iquote include -DHOST_TEST \
    tests/sched_host_test.cpp process/sched_policy.cpp \
    -o "$HOST_TOOLS/sched_host_test"
"$HOST_TOOLS/sched_host_test"

step "9/9" "Release verification"
"$PYTHON" tests/verify_release.py

printf '\n==================================\n'
printf ' BUILD SUCCESS (v2.0 + nano)\n'
printf '==================================\n\n'
printf ' Kernel: build/kernel.elf (AArch64)\n'
printf ' Disk : storage/disk.img\n'
printf ' Apps : /bin/init.elf + /bin/nano.elf\n'
printf ' Tests : FS PASS | NET PASS | MEM PASS | SCHED PASS | SYSCALL PASS | DIAG PASS\n\n'

if [[ "$RUN_QEMU" -eq 1 ]]; then
    if command -v "${QEMU:-qemu-system-aarch64}" >/dev/null 2>&1; then
        exec ./run.sh
    else
        echo "ERROR: QEMU not found. Build succeeded, but --run was requested." >&2
        exit 1
    fi
fi