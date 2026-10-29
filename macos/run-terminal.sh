#!/usr/bin/env bash
# Run RioOS and show its shell in a QEMU window (framebuffer terminal).
#
#   run.sh           -> RioOS in this host terminal (serial, -nographic)
#   run-terminal.sh  -> RioOS in a QEMU window: only the shell, black
#                       background, DejaVu Sans Mono font, scrollback with the
#                       mouse wheel (or Shift+PageUp / Shift+PageDown).
#
# The window is a plain text terminal drawn by the kernel on a QEMU ramfb
# surface. Keyboard and wheel input come from VirtIO keyboard + tablet devices
# (the absolute tablet needs no mouse grab, so the wheel works as soon as the
# pointer is over the window). The serial console also stays attached to this
# host terminal, so boot messages remain visible here.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_ROOT"

QEMU="${QEMU:-qemu-system-aarch64}"
KERNEL="${KERNEL:-build/kernel.elf}"
DISK="${DISK:-storage/disk.img}"
MEM="${RIOOS_MEM:-128M}"
CPU="${RIOOS_CPU:-cortex-a72}"
DISPLAY_BACKEND="${RIOOS_DISPLAY:-cocoa}"

command -v "$QEMU" >/dev/null 2>&1 || {
    echo "ERROR: qemu-system-aarch64 not found." >&2
    echo "  macOS: install QEMU with Homebrew or another trusted package source." >&2
    echo "  The QEMU build must include a Cocoa or SDL display backend." >&2
    exit 1
}
[[ -f "$KERNEL" ]] || { echo "ERROR: $KERNEL not found. Run the build script for this platform first." >&2; exit 1; }
[[ -f "$DISK" ]] || { echo "ERROR: $DISK not found. Run the build script for this platform first." >&2; exit 1; }

# A window needs a desktop session (macOS: Cocoa).
HOST_KIND="$(uname -s 2>/dev/null || true)"
IS_NATIVE_WINDOWS=0
IS_MACOS=0
case "$HOST_KIND" in
    Darwin) IS_MACOS=1 ;;
    MINGW*|MSYS*|CYGWIN*) IS_NATIVE_WINDOWS=1 ;;
esac
if [[ -z "${DISPLAY:-}" && -z "${WAYLAND_DISPLAY:-}" && "$IS_NATIVE_WINDOWS" -eq 0 && "$IS_MACOS" -eq 0 ]]; then
    echo "ERROR: no graphical session detected (DISPLAY / WAYLAND_DISPLAY is empty)." >&2
    echo "  Run this from a terminal inside your desktop, or use run.sh for a plain terminal." >&2
    exit 1
fi

# Arch's base qemu packages ship without GUI backends; detect that early
# instead of letting QEMU open no window at all.
backend_ok() { "$QEMU" -display help 2>&1 | grep -qw -- "$1"; }
if ! backend_ok "$DISPLAY_BACKEND"; then
    picked=""
    for candidate in cocoa sdl gtk; do
        if backend_ok "$candidate"; then picked="$candidate"; break; fi
    done
    if [[ -z "$picked" ]]; then
        echo "ERROR: this QEMU build has no usable Cocoa/SDL display backend." >&2
        echo "  Install a macOS QEMU build that includes the Cocoa or SDL display backend." >&2
        exit 1
    fi
    echo "NOTE: display backend '$DISPLAY_BACKEND' unavailable; using '$picked'." >&2
    DISPLAY_BACKEND="$picked"
fi

DEVICE_HELP="$("$QEMU" -device help 2>&1 || true)"
for required_device in ramfb virtio-keyboard-device virtio-tablet-device; do
    if ! grep -Fq "name \"${required_device}\"" <<<"$DEVICE_HELP"; then
        echo "ERROR: this QEMU build lacks '${required_device}'." >&2
        echo "  Install a QEMU system-aarch64 build that includes ramfb and VirtIO input devices." >&2
        exit 1
    fi
done

echo "RioOS terminal window: QEMU ($DISPLAY_BACKEND). Type in the window; scroll with the mouse wheel." >&2

exec "$QEMU" \
    -M virt,gic-version=2,virtualization=on \
    -global virtio-mmio.force-legacy=false \
    -cpu "$CPU" \
    -m "$MEM" \
    -display "$DISPLAY_BACKEND" \
    -serial stdio \
    -monitor none \
    -kernel "$KERNEL" \
    -device ramfb \
    -drive "if=none,format=raw,file=$DISK,id=vd0" \
    -device virtio-blk-device,drive=vd0 \
    -netdev user,id=n1,ipv4=on,ipv6=off \
    -device virtio-net-device,netdev=n1 \
    -device virtio-keyboard-device \
    -device virtio-tablet-device
