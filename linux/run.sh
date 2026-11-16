#!/usr/bin/env bash
set -euo pipefail

# Optional port forwards host->guest for httpd/ftpd/ssh servers, e.g.
#   RIOOS_FWD="tcp::8080-:8080,tcp::2121-:21" bash linux/run.sh
# (comma separated hostfwd rules; unset = no forwards, so no host port can clash)
RIOOS_FWD="${RIOOS_FWD-tcp::8080-:8080,tcp::2121-:21}"   # default as before; RIOOS_FWD="" disables
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

command -v "$QEMU" >/dev/null 2>&1 || {
    echo "ERROR: qemu-system-aarch64 not found."
    exit 1
}

[[ -f "$KERNEL" ]] || { echo "ERROR: $KERNEL not found. Run the build script for this platform first."; exit 1; }
[[ -f "$DISK" ]] || { echo "ERROR: $DISK not found. Run the build script for this platform first."; exit 1; }

# ---- optional extra devices (everything below is off by default) --------------------------------------------------
#   RIOOS_USB=1             xHCI controller + USB keyboard + USB tablet (hubs / hot-plug: use the QEMU monitor, see docs/DEVICES.md)
#   RIOOS_SOUND=<driver>    virtio-sound with a host audio driver (e.g. pa, alsa, sdl, wav); 1 = pa; wav writes ${RIOOS_WAV:-rioos-audio.wav}
#   RIOOS_9P=<dir>[,<dir2>] share host directories over virtio-9p (in the guest, as root: mkdir -p /mnt/p9; mount9p share0 /mnt/p9)
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
    -nographic \
    -monitor none \
    -kernel "$KERNEL" \
    -drive if=none,format=raw,file="$DISK",id=vd0 \
    -device virtio-blk-device,drive=vd0 \
    -device virtio-rng-device \
    -netdev "user,id=n1,ipv4=on,ipv6=off${NETFWD}" \
    -device virtio-net-device,netdev=n1 \
    ${EXTRA[@]+"${EXTRA[@]}"}
