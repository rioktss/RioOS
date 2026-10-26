#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_ROOT"

VERBOSE=0
RUN_QEMU=0
CLEAN=0

for arg in "$@"; do
    case "$arg" in
        -v|--verbose) VERBOSE=1 ;;
        --run) RUN_QEMU=1 ;;
        --clean) CLEAN=1 ;;
        -h|--help)
            cat <<EOF
Usage: bash ${BASH_SOURCE[0]} [options]
  --run      Build + tests + start the serial QEMU runner
  -v, --verbose  Show detailed compiler output
  --clean    Remove generated build/host-test outputs
EOF
            exit 0
            ;;
        *) echo "ERROR: unknown argument: $arg" >&2; exit 2 ;;
    esac
done

# cuma print kalo verbose
vlog() { [[ $VERBOSE -eq 1 ]] && printf '%s\n' "$*" || true; }
# step jadi diam kalo gak verbose
step() { [[ $VERBOSE -eq 1 ]] && printf '[%s] %s\n' "$1" "$2" || true; }

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
    rm -f build/*.o build/kernel.elf build/user_init.elf build/nano.elf build/calc.elf
    rm -f "$HOST_TOOLS"/fs_host_test
    rm -f "$HOST_TOOLS"/net_host_test
    rm -f "$HOST_TOOLS"/mem_host_test
    rm -f "$HOST_TOOLS"/syscall_host_test
    rm -f "$HOST_TOOLS"/diag_host_test
    rm -f "$HOST_TOOLS"/vfs_host_test
    rm -f "$HOST_TOOLS"/env_host_test
    rm -f "$HOST_TOOLS"/fd_host_test
    rm -f "$HOST_TOOLS"/process_host_test
    rm -f "$HOST_TOOLS"/scheduler_process_host_test
    rm -f "$HOST_TOOLS"/smp_host_test "$HOST_TOOLS"/fb_host_test "$HOST_TOOLS"/ui_host_test "$HOST_TOOLS"/ui_image_host_test
fi

src_of() {
    local f="$1" d
    case "$f" in
        ui_boot) printf '%s' 'ui/boot.cpp'; return ;;
        ui_lock_screen) printf '%s' 'ui/lock_screen.cpp'; return ;;
        ui_home_screen) printf '%s' 'ui/home_screen.cpp'; return ;;
        ui_apps) printf '%s' 'ui/apps.cpp'; return ;;
        ui_terminal) printf '%s' 'ui/terminal.cpp'; return ;;
        ui_calculator) printf '%s' 'ui/calculator.cpp'; return ;;
        ui_clock) printf '%s' 'ui/clock.cpp'; return ;;
        ui_file_manager) printf '%s' 'ui/file_manager.cpp'; return ;;
        ui_launcher) printf '%s' 'ui/launcher.cpp'; return ;;
        ui_wallpaper) printf '%s' 'ui/wallpaper.cpp'; return ;;
        ui_settings) printf '%s' 'ui/settings.cpp'; return ;;
        ui_image) printf '%s' 'ui/image.cpp'; return ;;
        ui_core) printf '%s' 'ui/ui.cpp'; return ;;
    esac
    for d in kernel drivers memory process fs net ui; do
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
    uart memory string fs vfs process sched_policy scheduler timer interrupt keyboard
    commands version shell history env rtc exceptions syscall user mmu virtio_mmio virtio_input virtio_blk storage thread fd
    fb font ui_font_data
    elf virtio_net netstack page_alloc fault diag security smp
    ui_boot ui_lock_screen ui_home_screen ui_apps ui_terminal ui_calculator ui_clock ui_file_manager ui_launcher ui_wallpaper ui_settings ui_image ui_core
    kernel
)

# MINIMAL PRINT CUMA INI
printf 'RioOS build...\n'

step "1/9" "Bundling userspace (init + nano + calc)"
"$CLANG" "${ASMFLAGS[@]}" -c userspace/user_init.S -o build/user_init.o
"$LD" -T userspace/user_init.ld -nostdlib -z max-page-size=0x1000 \
    build/user_init.o -o build/user_init.elf

if [[ -f userspace/nano.S ]]; then
    "$CLANG" "${ASMFLAGS[@]}" -c userspace/nano.S -o build/nano.o
    "$LD" -T userspace/user_init.ld -nostdlib -z max-page-size=0x1000 \
        build/nano.o -o build/nano.elf
    vlog " -> build/nano.elf ready"
else
    cp build/user_init.elf build/nano.elf 2>/dev/null || true
fi

if [[ -f userspace/calc.cpp ]]; then
    CALC_CXXFLAGS=(
        -Iinclude
        --target=aarch64-none-elf -mcpu=cortex-a72
        -ffreestanding -fno-exceptions -fno-rtti -fno-stack-protector -fno-builtin
        -fno-pic -fno-pie -fno-vectorize -fno-slp-vectorize
        -fno-unwind-tables -fno-asynchronous-unwind-tables
        -nostdinc++ -nostdlib -std=c++17 -O1 -Wall -Wextra -Werror
    )
    "$CXX" "${CALC_CXXFLAGS[@]}" -c userspace/calc.cpp -o build/calc.o
    "$LD" -T userspace/user_init.ld -nostdlib -z max-page-size=0x1000 \
        build/calc.o -o build/calc.elf
    vlog " -> build/calc.elf ready"
fi

step "2/9" "Building host tools"
"$CXX" -std=c++17 -O2 -Wall -Wextra -pedantic \
    fs/mkfs_myfs.cpp -o "$HOST_TOOLS/mkfs_myfs"

step "3/9" "Preparing disk image"
if [[ ! -f storage/disk.img ]]; then
    dd if=/dev/zero of=storage/disk.img bs=512 count=32768 2>/dev/null
fi

APP_ELFS=(build/user_init.elf)
[[ -f build/nano.elf ]] && APP_ELFS+=(build/nano.elf)
[[ -f build/calc.elf ]] && APP_ELFS+=(build/calc.elf)
APP_PACKAGES=(apps/terminal.rpk apps/calculator.rpk apps/clock.rpk apps/files.rpk apps/launcher.rpk apps/wallpaper.rpk apps/pengaturan.rpk)
for package in "${APP_PACKAGES[@]}"; do
    [[ -f "$package" ]] || { echo "ERROR: required UI package missing: $package" >&2; exit 1; }
done
LOCAL_ASSETS=()
for asset in assets/wallpapers/*.bmp assets/icons/*.bmp; do
    [[ -f "$asset" ]] && LOCAL_ASSETS+=("$asset")
done
"$HOST_TOOLS/mkfs_myfs" storage/disk.img "${APP_ELFS[@]}" "${APP_PACKAGES[@]}" "${LOCAL_ASSETS[@]}" >/dev/null
vlog " -> storage/disk.img ready"

step "4/9" "Compiling boot"
"$CLANG" "${ASMFLAGS[@]}" -c boot/boot.S -o build/boot.o
"$CLANG" "${ASMFLAGS[@]}" -c boot/exceptions.S -o build/exception_vectors.o
"$CLANG" "${ASMFLAGS[@]}" -c process/user_entry.S -o build/user_entry.o
"$CLANG" "${ASMFLAGS[@]}" -c process/ctx_switch.S -o build/ctx_switch.o

step "5/9" "Compiling kernel (${#MODULES[@]} modules)"
for f in "${MODULES[@]}"; do
    if [[ "$VERBOSE" -eq 1 ]]; then
        echo " -> $f.cpp"
        "$CXX" "${CXXFLAGS[@]}" -c "$(src_of "$f")" -o "build/$f.o"
    else
        if ! "$CXX" "${CXXFLAGS[@]}" -c "$(src_of "$f")" -o "build/$f.o"; then
            echo
            echo "ERROR: gagal compile $f.cpp" >&2
            exit 1
        fi
    fi
done

step "6/9" "Linking"
"$LD" -T boot/linker.ld -nostdlib -z max-page-size=0x1000 \
    build/boot.o build/exception_vectors.o build/user_entry.o build/ctx_switch.o \
    build/kernel.o build/uart.o build/memory.o build/string.o build/fs.o build/vfs.o \
    build/process.o build/sched_policy.o build/scheduler.o build/timer.o build/interrupt.o build/keyboard.o \
    build/commands.o build/version.o build/shell.o build/history.o build/env.o build/rtc.o build/exceptions.o build/syscall.o build/user.o build/mmu.o \
    build/virtio_mmio.o build/virtio_input.o build/virtio_blk.o build/storage.o build/elf.o build/virtio_net.o \
    build/netstack.o build/page_alloc.o build/fault.o build/diag.o build/security.o \
    build/fb.o build/font.o build/ui_font_data.o build/thread.o build/fd.o build/smp.o \
    build/ui_boot.o build/ui_lock_screen.o build/ui_home_screen.o build/ui_apps.o \
    build/ui_terminal.o build/ui_calculator.o build/ui_clock.o build/ui_file_manager.o \
    build/ui_launcher.o build/ui_wallpaper.o build/ui_settings.o build/ui_image.o build/ui_core.o \
    -o build/kernel.elf

step "7/9" "Verifying ELF"
if [[ "$VERBOSE" -eq 1 ]]; then
    if command -v "$READELF" >/dev/null 2>&1; then
        "$READELF" -h build/kernel.elf | grep -E 'Class:|Machine:|Entry point' || true
    fi
fi

step "8/9" "Running host tests"
"$CXX" -std=c++17 -O2 -Wall -Wextra -pedantic -iquote include -DHOST_TEST \
    tests/fs_host_test.cpp fs/fs.cpp kernel/string.cpp kernel/rtc.cpp \
    -o "$HOST_TOOLS/fs_host_test"
"$HOST_TOOLS/fs_host_test" > /dev/null

"$CXX" -std=c++17 -O2 -Wall -Wextra -Werror -pedantic -iquote include -DHOST_TEST \
    tests/net_host_test.cpp net/netstack.cpp \
    -o "$HOST_TOOLS/net_host_test"
"$HOST_TOOLS/net_host_test" > /dev/null

"$CXX" -std=c++17 -O2 -Wall -Wextra -pedantic -iquote include -DHOST_TEST \
    tests/mem_host_test.cpp memory/page_alloc.cpp kernel/fault.cpp \
    -o "$HOST_TOOLS/mem_host_test"
"$HOST_TOOLS/mem_host_test" > /dev/null

"$CXX" -std=c++17 -O2 -Wall -Wextra -pedantic -iquote include -DHOST_TEST \
    tests/syscall_host_test.cpp kernel/syscall.cpp kernel/fd.cpp \
    -o "$HOST_TOOLS/syscall_host_test"
"$HOST_TOOLS/syscall_host_test" > /dev/null

"$CXX" -std=c++17 -O2 -Wall -Wextra -pedantic -iquote include -DHOST_TEST \
    tests/diag_host_test.cpp kernel/diag.cpp \
    -o "$HOST_TOOLS/diag_host_test"
"$HOST_TOOLS/diag_host_test" > /dev/null

"$CXX" -std=c++17 -O2 -Wall -Wextra -Werror -pedantic -iquote include -DHOST_TEST \
    tests/vfs_host_test.cpp fs/vfs.cpp fs/fs.cpp kernel/string.cpp kernel/rtc.cpp \
    -o "$HOST_TOOLS/vfs_host_test"
"$HOST_TOOLS/vfs_host_test" > /dev/null

"$CXX" -std=c++17 -O2 -Wall -Wextra -Werror -pedantic -iquote include -DHOST_TEST \
    tests/sched_host_test.cpp process/sched_policy.cpp \
    -o "$HOST_TOOLS/sched_host_test"
"$HOST_TOOLS/sched_host_test" > /dev/null

"$CXX" -std=c++17 -O2 -Wall -Wextra -Werror -pedantic -iquote include -DHOST_TEST \
    tests/env_host_test.cpp kernel/env.cpp kernel/string.cpp \
    -o "$HOST_TOOLS/env_host_test"
"$HOST_TOOLS/env_host_test" > /dev/null

"$CXX" -std=c++17 -O2 -Wall -Wextra -Werror -pedantic -iquote include -DHOST_TEST \
    tests/fd_host_test.cpp kernel/fd.cpp fs/fs.cpp kernel/string.cpp kernel/rtc.cpp \
    -o "$HOST_TOOLS/fd_host_test"
"$HOST_TOOLS/fd_host_test" > /dev/null

"$CXX" -std=c++17 -O2 -Wall -Wextra -Werror -pedantic -iquote include -DHOST_TEST \
    tests/process_host_test.cpp process/process.cpp kernel/string.cpp \
    -o "$HOST_TOOLS/process_host_test"
"$HOST_TOOLS/process_host_test" > /dev/null

"$CXX" -std=c++17 -O2 -Wall -Wextra -Werror -pedantic -iquote include -DHOST_TEST \
    tests/scheduler_process_host_test.cpp process/scheduler.cpp process/process.cpp kernel/string.cpp \
    -o "$HOST_TOOLS/scheduler_process_host_test"
"$HOST_TOOLS/scheduler_process_host_test" > /dev/null

"$CXX" -std=c++17 -O2 -Wall -Wextra -Werror -pedantic -iquote include -DHOST_TEST \
    tests/smp_host_test.cpp kernel/smp.cpp \
    -o "$HOST_TOOLS/smp_host_test"
"$HOST_TOOLS/smp_host_test" > /dev/null

"$CXX" -std=c++17 -O2 -Wall -Wextra -Werror -pedantic -iquote include -DHOST_TEST \
    tests/fb_host_test.cpp drivers/fb.cpp drivers/font.cpp drivers/ui_font_data.cpp \
    -o "$HOST_TOOLS/fb_host_test"
"$HOST_TOOLS/fb_host_test" > /dev/null

"$CXX" -std=c++17 -O2 -Wall -Wextra -Werror -pedantic -iquote include -DHOST_TEST \
    tests/ui_host_test.cpp drivers/fb.cpp drivers/font.cpp drivers/ui_font_data.cpp ui/boot.cpp ui/lock_screen.cpp ui/home_screen.cpp \
    -o "$HOST_TOOLS/ui_host_test"
"$HOST_TOOLS/ui_host_test" > /dev/null

"$CXX" -std=c++17 -O2 -Wall -Wextra -Werror -pedantic -iquote include -DHOST_TEST \
    tests/ui_image_host_test.cpp drivers/fb.cpp drivers/font.cpp drivers/ui_font_data.cpp ui/image.cpp ui/settings.cpp kernel/string.cpp \
    -o "$HOST_TOOLS/ui_image_host_test"
"$HOST_TOOLS/ui_image_host_test" > /dev/null

step "9/9" "Release verification"
"$PYTHON" tests/verify_release.py > /dev/null

# MINIMAL SUCCESS
printf 'build success\n'

if [[ "$RUN_QEMU" -eq 1 ]]; then
    if command -v "${QEMU:-qemu-system-aarch64}" >/dev/null 2>&1; then
        exec "$SCRIPT_DIR/run.sh"
    else
        echo "ERROR: QEMU not found." >&2
        exit 1
    fi
fi