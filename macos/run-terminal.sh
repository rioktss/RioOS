#!/usr/bin/env bash
# Start the original RioOS UART shell in a plain black QEMU text-console window.
# This runner deliberately does NOT attach ramfb, fw_cfg start-terminal, or
# VirtIO GUI input devices, so the pixel UI and its .rpk Terminal app stay off.
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

# Linux/X11/Wayland and Termux:X11 need a graphical session. Native Windows
# can open the SDL window without DISPLAY; WSLg provides DISPLAY/WAYLAND_DISPLAY.
HOST_KIND="$(uname -s 2>/dev/null || true)"
IS_NATIVE_WINDOWS=0
IS_MACOS=0
case "$HOST_KIND" in
    Darwin) IS_MACOS=1 ;;
    MINGW*|MSYS*|CYGWIN*) IS_NATIVE_WINDOWS=1 ;;
esac
if [[ -z "${DISPLAY:-}" && -z "${WAYLAND_DISPLAY:-}" && "$IS_NATIVE_WINDOWS" -eq 0 && "$IS_MACOS" -eq 0 ]]; then
    echo "ERROR: no graphical session detected (DISPLAY / WAYLAND_DISPLAY is empty)." >&2
    echo "  On Android, start Termux:X11 first; on WSL use WSLg." >&2
    echo "  On a Windows laptop, use run-terminal.ps1 or run this from Git Bash with SDL QEMU." >&2
    exit 1
fi

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

echo "RioOS text shell: opening a plain QEMU console window ($DISPLAY_BACKEND)." >&2
echo "The window shows the original UART command shell on a black background." >&2
echo "This mode does not start the pixel UI or the Terminal .rpk app." >&2

# QEMU's virtual console presents UART text in the graphical window. No ramfb
# is attached, so ui_start() leaves the built-in serial shell as the only UI.
# Keep the virtual disk and VirtIO network devices from the normal runner.
exec "$QEMU" \
    -M virt,gic-version=2,virtualization=on \
    -global virtio-mmio.force-legacy=false \
    -cpu "$CPU" \
    -m "$MEM" \
    -display "$DISPLAY_BACKEND" \
    -serial vc:100Cx32C \
    -monitor none \
    -kernel "$KERNEL" \
    -drive "if=none,format=raw,file=$DISK,id=vd0" \
    -device virtio-blk-device,drive=vd0 \
    -netdev user,id=n1,ipv4=on,ipv6=off \
    -device virtio-net-device,netdev=n1
