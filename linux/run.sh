#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_ROOT"

QEMU="${QEMU:-qemu-system-aarch64}"
KERNEL="${KERNEL:-build/kernel.elf}"
DISK="${DISK:-storage/disk.img}"
MEM="${RIOOS_MEM:-128M}"
CPU="${RIOOS_CPU:-cortex-a72}"

command -v "$QEMU" >/dev/null 2>&1 || {
    echo "ERROR: qemu-system-aarch64 not found."
    exit 1
}

[[ -f "$KERNEL" ]] || { echo "ERROR: $KERNEL not found. Run the build script for this platform first."; exit 1; }
[[ -f "$DISK" ]] || { echo "ERROR: $DISK not found. Run the build script for this platform first."; exit 1; }

exec "$QEMU" \
    -M virt,gic-version=2,virtualization=on \
    -global virtio-mmio.force-legacy=false \
    -cpu "$CPU" \
    -m "$MEM" \
    -nographic \
    -monitor none \
    -kernel "$KERNEL" \
    -drive if=none,format=raw,file="$DISK",id=vd0 \
    -device virtio-blk-device,drive=vd0 \
    -netdev user,id=n1,ipv4=on,ipv6=off \
    -device virtio-net-device,netdev=n1