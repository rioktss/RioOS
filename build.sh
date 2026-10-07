#!/data/data/com.termux/files/usr/bin/bash
set -euo pipefail
cd "$(dirname "$0")"

VERBOSE=0
[[ "${1:-}" == "-v" || "${1:-}" == "--verbose" ]] && VERBOSE=1

log() { printf '%s\n' "$*"; }
vlog() { [[ $VERBOSE -eq 1 ]] && printf '%s\n' "$*" || true; }
step() { printf '[%s] %s\n' "$1" "$2"; }

printf '\n==================================\n'
printf '       RioOS Build System v1.5.1\n'
printf '==================================\n\n'

CLANG="${CLANG:-clang}"
CXX="${CXX:-clang++}"
LD="${LD:-ld.lld}"
QEMU="${QEMU:-qemu-system-aarch64}"

command -v "$CLANG" >/dev/null 2>&1 || { echo 'ERROR: clang not found'; exit 1; }
command -v "$CXX" >/dev/null 2>&1 || { echo 'ERROR: clang++ not found'; exit 1; }
command -v "$LD" >/dev/null 2>&1 || { echo 'ERROR: ld.lld not found'; exit 1; }

mkdir -p build storage storage/home
HOST_TOOLS="$HOME/.mykernel-tools"
mkdir -p "$HOST_TOOLS"

rm -f build/*.o build/kernel.elf build/user_init.elf
rm -f "$HOST_TOOLS/fs_host_test" "$HOST_TOOLS/net_host_test" "$HOST_TOOLS/mem_host_test" "$HOST_TOOLS/syscall_host_test"

# Map module name -> source path (P1 directory layout).
src_of() {
    local f="$1" d
    for d in kernel drivers memory process fs net; do
        [[ -f "$d/$f.cpp" ]] && { printf '%s' "$d/$f.cpp"; return; }
    done
    echo "ERROR: source for $f not found" >&2; exit 1
}

CXXFLAGS=(
    -Iinclude
    --target=aarch64-none-elf -mcpu=cortex-a72 -mgeneral-regs-only -mstrict-align
    -ffreestanding -fno-exceptions -fno-rtti -fno-stack-protector -fno-builtin
    -fno-pic -fno-pie -fno-vectorize -fno-slp-vectorize
    -fno-unwind-tables -fno-asynchronous-unwind-tables
    -nostdinc++ -std=c++17 -O1 -Wall -Wextra -Werror
)
ASMFLAGS=( -Iinclude --target=aarch64-none-elf -mcpu=cortex-a72 -ffreestanding -fno-pic -fno-pie )

# [1/9] USERSPACE
step "1/9" "Bundling userspace"
vlog "  -> user_init.S"
"$CLANG" "${ASMFLAGS[@]}" -c userspace/user_init.S -o build/user_init.o
"$LD" -T userspace/user_init.ld -nostdlib -z max-page-size=0x1000 build/user_init.o -o build/user_init.elf

# [2/9] FORMATTER
step "2/9" "Building host tools"
"$CXX" -std=c++17 -O2 -Wall -Wextra -pedantic fs/mkfs_myfs.cpp -o "$HOST_TOOLS/mkfs_myfs"

# [3/9] DISK IMAGE
step "3/9" "Preparing disk image"
if [ ! -f storage/disk.img ]; then
    dd if=/dev/zero of=storage/disk.img bs=512 count=32768 status=none
fi
"$HOST_TOOLS/mkfs_myfs" storage/disk.img build/user_init.elf > /dev/null
vlog "  -> storage/disk.img ready (16 MiB)"

# [4/9] BOOT
step "4/9" "Compiling boot"
"$CLANG" "${ASMFLAGS[@]}" -c boot/boot.S -o build/boot.o
"$CLANG" "${ASMFLAGS[@]}" -c boot/exceptions.S -o build/exception_vectors.o
"$CLANG" "${ASMFLAGS[@]}" -c process/user_entry.S -o build/user_entry.o

# [5/9] KERNEL
step "5/9" "Compiling kernel (24 modules)"
if [[ $VERBOSE -eq 1 ]]; then
    for f in uart memory string fs vfs process scheduler timer interrupt keyboard commands shell exceptions syscall user mmu virtio_mmio virtio_blk storage elf virtio_net netstack page_alloc fault kernel; do
        echo "  -> $f.cpp"
        "$CXX" "${CXXFLAGS[@]}" -c "$(src_of "$f")" -o "build/$f.o"
    done
else
    for f in uart memory string fs vfs process scheduler timer interrupt keyboard commands shell exceptions syscall user mmu virtio_mmio virtio_blk storage elf virtio_net netstack page_alloc fault kernel; do
        printf '.'
        "$CXX" "${CXXFLAGS[@]}" -c "$(src_of "$f")" -o "build/$f.o" > /dev/null 2>&1
    done
    printf ' done\n'
fi

# [6/9] LINK
step "6/9" "Linking"
"$LD" -T linker.ld -nostdlib -z max-page-size=0x1000 \
    build/boot.o build/exception_vectors.o build/user_entry.o \
    build/kernel.o build/uart.o build/memory.o build/string.o build/fs.o build/vfs.o \
    build/process.o build/scheduler.o build/timer.o build/interrupt.o build/keyboard.o \
    build/commands.o build/shell.o build/exceptions.o build/syscall.o build/user.o build/mmu.o \
    build/virtio_mmio.o build/virtio_blk.o build/storage.o build/elf.o build/virtio_net.o build/netstack.o build/page_alloc.o build/fault.o \
    -o build/kernel.elf

# [7/9] VERIFY
step "7/9" "Verifying ELF"
if command -v llvm-readelf >/dev/null 2>&1; then
    llvm-readelf -h build/kernel.elf | grep -E 'Class:|Machine:|Entry point' || true
else
    vlog "  -> llvm-readelf not found, skip"
fi

# [8/9] TESTS
step "8/9" "Running host tests"
"$CXX" -std=c++17 -O2 -Wall -Wextra -pedantic -iquote include -DHOST_TEST tests/fs_host_test.cpp fs/fs.cpp kernel/string.cpp -o "$HOST_TOOLS/fs_host_test" > /dev/null
"$HOST_TOOLS/fs_host_test"

"$CXX" -std=c++17 -O2 -Wall -Wextra -Werror -pedantic -iquote include -DHOST_TEST tests/net_host_test.cpp net/netstack.cpp -o "$HOST_TOOLS/net_host_test" > /dev/null
"$HOST_TOOLS/net_host_test"

"$CXX" -std=c++17 -O2 -Wall -Wextra -pedantic -iquote include -DHOST_TEST tests/mem_host_test.cpp memory/page_alloc.cpp kernel/fault.cpp -o "$HOST_TOOLS/mem_host_test" > /dev/null
"$HOST_TOOLS/mem_host_test"

"$CXX" -std=c++17 -O2 -Wall -Wextra -pedantic -iquote include -DHOST_TEST tests/syscall_host_test.cpp kernel/syscall.cpp -o "$HOST_TOOLS/syscall_host_test" > /dev/null
"$HOST_TOOLS/syscall_host_test"

python3 tests/verify_release.py

printf '\n==================================\n'
printf '          BUILD SUCCESS (v1.5.1)\n'
printf '==================================\n\n'
printf '  Kernel: build/kernel.elf (AArch64 @ 0x40100000)\n'
printf '  Disk  : storage/disk.img\n'
printf '  Tests : FS PASS | NET PASS | STATIC PASS\n\n'

if command -v "$QEMU" >/dev/null 2>&1; then
    printf 'Starting QEMU...\n\n'
    exec "$QEMU" -M virt,gic-version=2,virtualization=on -global virtio-mmio.force-legacy=false \
        -cpu cortex-a72 -m 128M -nographic -monitor none \
        -kernel build/kernel.elf \
        -drive if=none,format=raw,file=storage/disk.img,id=vd0 -device virtio-blk-device,drive=vd0 \
        -netdev user,id=n1,ipv4=on,ipv6=off -device virtio-net-device,netdev=n1
else
    printf 'QEMU not found. Build OK.\n'
fi