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

# Optional port forwards host->guest for httpd/ftpd/ssh servers, e.g.
#   RIOOS_FWD="tcp::8080-:8080,tcp::2121-:21" bash linux/run-terminal.sh
# (comma separated hostfwd rules; unset = no forwards, so no host port can clash)
NETFWD=""
if [ -n "${RIOOS_FWD:-}" ]; then
    IFS=',' read -ra _fw <<< "$RIOOS_FWD"
    for _r in "${_fw[@]}"; do NETFWD="${NETFWD},hostfwd=${_r}"; done
fi
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_ROOT"

QEMU="${QEMU:-qemu-system-aarch64}"
KERNEL="${KERNEL:-build/kernel.elf}"
DISK="${DISK:-storage/disk.img}"
MEM="${RIOOS_MEM:-128M}"
CPU="${RIOOS_CPU:-cortex-a72}"
DISPLAY_BACKEND="${RIOOS_DISPLAY:-gtk}"

command -v "$QEMU" >/dev/null 2>&1 || {
    echo "ERROR: qemu-system-aarch64 not found." >&2
    echo "  Arch Linux : sudo pacman -S qemu-system-aarch64 qemu-ui-gtk qemu-ui-sdl" >&2
    echo "  Debian/Ubuntu: sudo apt install qemu-system-arm" >&2
    echo "  Termux: use a QEMU build with GTK or SDL display support." >&2
    exit 1
}
[[ -f "$KERNEL" ]] || { echo "ERROR: $KERNEL not found. Run the build script for this platform first." >&2; exit 1; }
[[ -f "$DISK" ]] || { echo "ERROR: $DISK not found. Run the build script for this platform first." >&2; exit 1; }

# A window needs a desktop session (X11 / Wayland / WSLg / Termux:X11).
HOST_KIND="$(uname -s 2>/dev/null || true)"
IS_NATIVE_WINDOWS=0
case "$HOST_KIND" in MINGW*|MSYS*|CYGWIN*) IS_NATIVE_WINDOWS=1 ;; esac
if [[ -z "${DISPLAY:-}" && -z "${WAYLAND_DISPLAY:-}" && "$IS_NATIVE_WINDOWS" -eq 0 ]]; then
    echo "ERROR: no graphical session detected (DISPLAY / WAYLAND_DISPLAY is empty)." >&2
    echo "  Run this from a terminal inside your desktop, or use run.sh for a plain terminal." >&2
    exit 1
fi

# Arch's base qemu packages ship without GUI backends; detect that early
# instead of letting QEMU open no window at all.
backend_ok() { "$QEMU" -display help 2>&1 | grep -qw -- "$1"; }
if ! backend_ok "$DISPLAY_BACKEND"; then
    picked=""
    for candidate in gtk sdl; do
        if backend_ok "$candidate"; then picked="$candidate"; break; fi
    done
    if [[ -z "$picked" ]]; then
        echo "ERROR: this QEMU build has no usable GTK/SDL display backend." >&2
        echo "  Arch Linux : sudo pacman -S qemu-ui-gtk qemu-ui-sdl (or qemu-full)" >&2
        exit 1
    fi
    echo "NOTE: display backend '$DISPLAY_BACKEND' unavailable; using '$picked'." >&2
    DISPLAY_BACKEND="$picked"
fi

FBDEV=ramfb
if [ "${RIOOS_GPU:-}" = "virtio" ]; then FBDEV=virtio-gpu-device; fi
DEVICE_HELP="$("$QEMU" -device help 2>&1 || true)"
for required_device in "$FBDEV" virtio-keyboard-device virtio-tablet-device; do
    if ! grep -Fq "name \"${required_device}\"" <<<"$DEVICE_HELP"; then
        echo "ERROR: this QEMU build lacks '${required_device}'." >&2
        echo "  Install a QEMU system-aarch64 build that includes ramfb and VirtIO input devices." >&2
        exit 1
    fi
done

echo "RioOS terminal window: QEMU ($DISPLAY_BACKEND). Type in the window; scroll with the mouse wheel." >&2

# ---- optional extra devices (everything below is off by default) --------------------------------------------------
#   RIOOS_USB=1             xHCI controller + USB keyboard + USB tablet (hubs / hot-plug: use the QEMU monitor, see docs/DEVICES.md)
#   RIOOS_SOUND=<driver>    virtio-sound with a host audio driver (e.g. pa, alsa, sdl, wav); 1 = pa; wav writes ${RIOOS_WAV:-rioos-audio.wav}
#   RIOOS_9P=<dir>[,<dir2>] share host directories over virtio-9p (in the guest, as root: mkdir -p /mnt/p9; mount9p share0 /mnt/p9)
#   RIOOS_GPU=virtio        virtio-gpu (2D) instead of ramfb
need_dev() {
    "$QEMU" -device help 2>&1 | grep -Fq "name \"$1\"" || { echo "ERROR: this QEMU build lacks '$1'." >&2; exit 1; }
}
EXTRA=()
if [ -n "${RIOOS_USB:-}" ] && [ "${RIOOS_USB}" != "0" ]; then
    need_dev qemu-xhci; need_dev usb-kbd; need_dev usb-tablet
    EXTRA+=(-device qemu-xhci,id=xhci -device usb-kbd,bus=xhci.0 -device usb-tablet,bus=xhci.0)
fi
if [ -n "${RIOOS_SOUND:-}" ] && [ "${RIOOS_SOUND}" != "0" ]; then
    need_dev virtio-sound-device
    _snd="${RIOOS_SOUND}"; [ "$_snd" = "1" ] && _snd="pa"
    if [ "$_snd" = "wav" ]; then EXTRA+=(-audiodev "wav,id=snd0,path=${RIOOS_WAV:-rioos-audio.wav}"); else EXTRA+=(-audiodev "${_snd},id=snd0"); fi
    EXTRA+=(-device virtio-sound-device,audiodev=snd0)
fi
if [ -n "${RIOOS_9P:-}" ]; then
    need_dev virtio-9p-device
    IFS=',' read -ra _dirs <<< "$RIOOS_9P"
    _i=0
    for _d in "${_dirs[@]}"; do
        [ -d "$_d" ] || { echo "ERROR: RIOOS_9P directory '$_d' not found." >&2; exit 1; }
        EXTRA+=(-fsdev "local,id=fs${_i},path=${_d},security_model=none" -device "virtio-9p-device,fsdev=fs${_i},mount_tag=share${_i}")
        _i=$((_i + 1))
    done
fi

exec "$QEMU" \
    -M virt,gic-version=2,virtualization=on \
    -global virtio-mmio.force-legacy=false \
    -cpu "$CPU" \
    -m "$MEM" \
    -smp 4 \
    -display "$DISPLAY_BACKEND" \
    -serial stdio \
    -monitor none \
    -kernel "$KERNEL" \
    -device "$FBDEV" \
    -drive "if=none,format=raw,file=$DISK,id=vd0" \
    -device virtio-blk-device,drive=vd0 \
    -device virtio-rng-device \
    -netdev "user,id=n1,ipv4=on,ipv6=off${NETFWD}" \
    -device virtio-net-device,netdev=n1 \
    -device virtio-keyboard-device \
    -device virtio-tablet-device \
    ${EXTRA[@]+"${EXTRA[@]}"}
