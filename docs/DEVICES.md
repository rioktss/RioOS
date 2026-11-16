# Devices: USB HID, virtio-gpu, virtio-sound, virtio-9p (Round 11)

All four are polled drivers (no interrupts) for QEMU's `virt` machine. The virtio ones use the modern virtio-mmio v2
transport, so QEMU needs `-global virtio-mmio.force-legacy=false` (already in every run script). The shared transport
helper is `include/virtio_dev.h` / `drivers/virtio_dev.cpp`.

Run scripts (`run.sh`, `run-terminal.sh` for Linux, macOS, Termux, and the `.ps1` equivalents) accept optional switches; the
defaults are unchanged:

| variable | effect |
|---|---|
| `RIOOS_GPU=virtio` | virtio-gpu instead of ramfb (terminal scripts) |
| `RIOOS_USB=1` | `qemu-xhci` + `usb-kbd` + `usb-tablet` |
| `RIOOS_SOUND=<driver>` | `virtio-sound` with a host audio driver (`pa`, `alsa`, `coreaudio`, `dsound`, `sdl`, `wav`); `1` = platform default; `wav` writes `$RIOOS_WAV` (default `rioos-audio.wav`) |
| `RIOOS_9P=<dir>[,<dir2>]` | host folders as virtio-9p shares `share0`, `share1` (not on Windows: QEMU there has no virtfs) |

Example: `RIOOS_GPU=virtio RIOOS_USB=1 RIOOS_SOUND=1 RIOOS_9P=$HOME/shared bash linux/run-terminal.sh`

## USB (xHCI) — `drivers/usb.cpp`, commands `usb`, `usb scan`, `usb selftest`

QEMU: `-device qemu-xhci,id=x -device usb-kbd,bus=x.0 -device usb-tablet,bus=x.0` (also `usb-mouse`, `usb-hub`).

- **Keyboards** (boot protocol): Shift, Ctrl, Caps Lock and Num Lock (with the LEDs set on the keyboard), keypad (digits when Num Lock is on, arrows/Del when off),
  arrows, Delete, PgUp/PgDn (console scrollback, as with virtio-input), key repeat (500 ms delay, 40 ms rate). Arrow and PgUp/PgDn codes are the same 0x80..0x85 bytes virtio-input produces, so shell history and line editing work.
- **Pointing devices** are recognised through their **HID report descriptor** (not only the boot layout): relative mice with wheel and extra buttons, and
  absolute tablets/touch screens with any logical range (scaled to 0..32767). Events are delivered as `RioInputEvent` (`virtio_input_poll` falls back to `usb_poll_event`), the wheel scrolls the console like the virtio wheel.
- **Hubs**, several tiers deep (route string, hub descriptor, port power, reset, per-port status polling every 250 ms, transaction-translator fields for low/full-speed devices behind a high-speed hub).
- **Hot-plug and hot-unplug** on root ports (port-status-change events) and on hub ports; unplugging a hub removes everything behind it (Disable Slot). `usb scan` re-checks every port (also retries a port whose device was ignored or failed).
- **Composite devices**: up to 2 HID interfaces per device; non-HID interfaces/devices are listed as `(other)` and left alone.
- **Robustness**: EP0 max-packet size is read from the device and fixed with Evaluate Context; STALL / transaction errors on EP0 and on interrupt endpoints are recovered (Reset Endpoint + Set TR Dequeue Pointer), an endpoint that keeps failing is given up after 8 errors; PORTSC is written without disturbing PED/PR; 32- and 64-byte xHCI contexts.
- `usb` shows the device tree (`port 5.2.1` = root port 5 → hub port 2 → hub port 1), vendor:product ids, counters (key events, pointer events, hot-plug adds/removals, transfer errors and recoveries).

Limits: polling only (the shell idle loop calls `usb_poll`; a program that never reads input sees no hot-plug), 8 devices at once (extra ones are counted as "ignored"),
2 HID interfaces per device, no isochronous/bulk transfers (so no USB storage/audio/network; such devices are enumerated and listed as `(other)`).
Tested only against QEMU's xHCI: SuperSpeed *devices* enumerate (QEMU's `usb-storage` does), but QEMU has no SuperSpeed hub or SuperSpeed HID device, so the SuperSpeed hub path
(SET_HUB_DEPTH, 0x2A descriptor) follows the specification and is **untested**, as is everything on real hardware (no real xHCI board was available).
Report descriptors with several pointer collections use the first one that has X and Y.

## virtio-gpu — `drivers/virtio_gpu.cpp`, `fbinfo`

QEMU: `-device virtio-gpu-device` (`RIOOS_GPU=virtio`). 2D scanout of a 1024x768 B8G8R8X8 surface: resource create, attach backing, set scanout, transfer-to-host and flush only for the dirty rectangle.
`fb_init` prefers virtio-gpu over ramfb when both exist. Without a display device the kernel stays on the serial console.
Limits: 2D only (no 3D/virgl, no hardware cursor, no EDID/resize events), fixed 1024x768, scanout 0 only, polled (500 ms timeout per command; `fbinfo` shows the backend, the host scanout and the number of flushes).

## virtio-sound — `drivers/virtio_snd.cpp`, command `sound`

QEMU: `-audiodev <driver>,id=a -device virtio-sound-device,audiodev=a`.
`sound info`, `sound beep [freq] [ms] [vol]`, `sound play file.wav`. Output only, PCM S16. The stream is played at exactly the file's rate and channel count, so both must be offered by the device (`sound info` lists them; QEMU: 5.5-384 kHz, 1-2 channels);
anything else is refused with "unsupported or invalid rate/channels/length" (no resampling or channel conversion). The call blocks until the data was consumed by the host audio backend.
Limits: playback only (no capture, no mixing, no volume control except `beep`), WAV must be 16-bit PCM (plain or WAVE_FORMAT_EXTENSIBLE) and ≤ 1 MiB, one stream at a time, kernel command only (no userspace syscall).

## virtio-9p (9P2000.L) — `drivers/ninep.cpp`, `fs/fs9p.inc`, commands `9p`, `mount9p`, `mount -t 9p`

QEMU (needs a QEMU built with virtfs): `-fsdev local,id=f,path=DIR,security_model=none -device virtio-9p-device,fsdev=f,mount_tag=TAG`.
In the guest, as root: `mkdir -p /mnt/p9; mount9p TAG /mnt/p9` (`mount9p -r` = read-only; `umount /mnt/p9`). The share behaves like any other mount: `ls`, `cat`, `cp`, `mv`, `rm -r`, `ln -s`, `chmod`, `chown`, `touch`, editors, `tar`, userspace programs via the normal file API.

How it works: the server stays authoritative. The kernel keeps a mirror (cache) of the tree in a RAM device (`9p cache` 8.5 MiB per share): directories are refreshed when older than 1 s, every read checks the file's version/size/mtime on the server first, so changes made on the host appear in the guest within ~1 s;
guest changes are pushed to the server synchronously and, if the server refuses, rolled back by re-syncing from it.

Limits (all deliberate and reported by the tool, never silent corruption): at most 2 shares; at most 255 cached nodes per share (more entries are hidden and `mount` says how many); names shorter than 64 bytes; files up to 4 MiB; a 8.5 MiB cache per share (larger trees work with eviction, a single file must fit);
no hard links, no `fsck`; FIFOs, sockets and device nodes on the host are hidden; ownership is whatever the server reports (with `security_model=none` that is the host user, so guest `chown` may be refused by the server and is then rolled back);
a file changed twice within the same nanosecond-timestamp tick with identical size and version is not detected; the "hidden entries" count is approximate.

## Tests

`python3 tests/qemu_devices.py [qemu] [usb gpu sound 9p]` (also run by `tests/qemu_regression.py`; a session is skipped with a `SKIP` line when the QEMU build lacks the device — virtio-9p needs virtfs, virtio-sound needs the `wav` audio backend).
`tests/fs9p_host_test.cpp` (run by `linux/build.sh`) exercises the 9p mirror against an in-memory 9P server, also under ASan/UBSan.
