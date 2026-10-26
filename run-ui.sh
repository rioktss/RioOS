#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

QEMU="${QEMU:-qemu-system-aarch64}"
MEM="${RIOOS_MEM:-128M}"
CPU="${RIOOS_CPU:-cortex-a72}"
DISPLAY_BACKEND="${RIOOS_DISPLAY:-gtk}"

command -v "$QEMU" >/dev/null 2>&1 || {
    echo "ERROR: qemu-system-aarch64 not found." >&2
    echo "  Arch Linux : sudo pacman -S qemu-system-aarch64 qemu-ui-gtk qemu-ui-sdl" >&2
    echo "  Debian/Ubuntu: sudo apt install qemu-system-arm" >&2
    exit 1
}
[[ -f build/kernel.elf && -f storage/disk.img ]] || {
    echo "ERROR: build artifacts missing. Run ./build.sh first." >&2
    exit 1
}

# A window needs a desktop session (X11 or Wayland).
if [[ -z "${DISPLAY:-}" && -z "${WAYLAND_DISPLAY:-}" && "$DISPLAY_BACKEND" =~ ^(gtk|sdl)$ ]]; then
    echo "ERROR: no graphical session (DISPLAY / WAYLAND_DISPLAY is empty)." >&2
    echo "  Run this from a terminal inside your desktop, not from a TTY or plain SSH." >&2
    exit 1
fi

# Arch's base qemu packages ship without GUI backends; detect that early instead
# of letting QEMU open no window at all.
backend_ok() { "$QEMU" -display help 2>&1 | grep -qw -- "$1"; }
if ! backend_ok "$DISPLAY_BACKEND"; then
    picked=""
    for candidate in gtk sdl; do
        if backend_ok "$candidate"; then picked="$candidate"; break; fi
    done
    if [[ -z "$picked" ]]; then
        echo "ERROR: this QEMU has no '$DISPLAY_BACKEND' (or gtk/sdl) display backend." >&2
        echo "  Arch Linux : sudo pacman -S qemu-ui-gtk qemu-ui-sdl   (or qemu-full)" >&2
        exit 1
    fi
    echo "NOTE: display backend '$DISPLAY_BACKEND' unavailable, using '$picked'." >&2
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

# A relative mouse carries X11 / trackpad wheel events. Keep tablet-only QEMU
# builds working too; touch/click controls remain usable without wheel support.
MOUSE_DEVICE_ARGS=()
if grep -Fq 'name "virtio-mouse-device"' <<<"$DEVICE_HELP"; then
    MOUSE_DEVICE_ARGS=(-device virtio-mouse-device)
else
    echo "WARNING: virtio-mouse-device is unavailable; wheel scrolling will be disabled, but tablet input remains enabled." >&2
fi

echo "RioOS UI: window = QEMU ($DISPLAY_BACKEND)." >&2
echo "Use keyboard, touch/tablet, or mouse wheel: swipe up to unlock, click an app, scroll lists/output." >&2
echo "Serial input remains available in this terminal as a fallback (U unlocks, WASD moves, Enter opens)." >&2

# ramfb is the framebuffer; VirtIO input transports deliver keyboard/tablet and, when available, mouse-wheel events.
exec "$QEMU" \
    -M virt,gic-version=2,virtualization=on \
    -global virtio-mmio.force-legacy=false \
    -cpu "$CPU" \
    -m "$MEM" \
    -display "$DISPLAY_BACKEND" \
    -serial stdio \
    -monitor none \
    -kernel build/kernel.elf \
    -device ramfb \
    -drive if=none,format=raw,file=storage/disk.img,id=vd0 \
    -device virtio-blk-device,drive=vd0 \
    -netdev user,id=n1,ipv4=on,ipv6=off \
    -device virtio-net-device,netdev=n1 \
    -device virtio-keyboard-device \
    -device virtio-tablet-device \
    "${MOUSE_DEVICE_ARGS[@]}"
