# RioOS Pixel UI

RioOS v1.11.0 adds an optional pixel-rendered interface for QEMU `virt` using QEMU's `ramfb` device and the `fw_cfg` interface. The renderer is freestanding C++ and direct pixel primitives and a compact antialiased proportional font table for the GUI; it does not use SDL, Qt, LVGL, a third-party GUI library, or ASCII-art UI. The serial console keeps its legacy bitmap font.

## Build and launch

Linux / Termux with a compatible AArch64 cross-build toolchain and QEMU:

```sh
bash build.sh --clean
./run-ui.sh
```

Windows PowerShell:

```powershell
.\build.ps1 --clean
.\run-ui.ps1
```

The graphical UI runner defaults to GTK; override it with `RIOOS_DISPLAY=sdl ./run-ui.sh` when SDL is supported. `./run-ui.sh` opens the RioOS lock/home UI and its built-in apps. For a separate plain black text shell, run `./run-terminal.sh` (or `RIOOS_DISPLAY=sdl ./run-terminal.sh` when SDL is preferred); on Windows PowerShell use `./run-terminal.ps1`. This text-only runner sends the original UART shell to QEMU's virtual console and deliberately does not attach `ramfb` or request the pixel Terminal app. The graphical UI runner requests a 1024x768 XRGB8888 ram framebuffer. The firmware config interface is probed at boot; if no compatible ramfb device is found, RioOS reports that fact over UART and continues into the existing serial shell rather than hanging while waiting for a display.

## Screens

1. **Boot splash** — large white `RioOS` lettering centered on a dark pixel background.
2. **Lock screen** — gradient wallpaper, large clock and unlock prompt.
3. **Home screen** — status area, wallpaper, app icons, dock, and launcher navigation.
4. **Built-in applications** — Terminal, Calculator, Clock, Files, Launcher, Wallpaper, and Settings. Wallpaper and icon assets are stored locally in MyFS and referenced by app manifests.

## Input

The graphical runner attaches a VirtIO keyboard and absolute-position tablet; when supported by the QEMU build, it also attaches a relative mouse so desktop or Termux:X11 trackpad wheel events reach the guest. Focus the QEMU window to send keyboard input. Swipe/drag upward on the lock screen to unlock, click app controls, and use the wheel or vertical drag gestures to move through File Manager entries, Wallpaper choices, Launcher selection, and Terminal output. If QEMU lacks the relative mouse device, the runner warns but keeps the existing keyboard/tablet UI working. UART/serial input remains enabled as a fallback.

| Screen | QEMU-window / fallback input | Action |
|---|---|---|
| Lock | Swipe/drag upward; any serial byte still unlocks | Open home screen |
| Home | Click an app icon; `A` / `D` or arrow keys; `Enter` or `1`–`7` | Select/open app |
| Home | Swipe down from the top edge or press `C` | Open Control Centre |
| Control Centre | Click a tile or press `1`–`6`; `C` / `Esc` closes | Toggle theme, open Wallpaper/Settings, lock, or close panel |
| Home | `W` / `S`; `K`; `X` | Move vertically; lock; return to serial shell |
| Apps | Click the top-left header control or press `Esc` | Return home |
| Terminal | Type command then `Enter`; `Up` / `Down` browse history; `Ctrl+S` / `Ctrl+X` are passed to interactive Nano; `Esc` returns home | Run RioOS shell commands or use Nano |
| Calculator | Click keypad or type expression then `Enter` | Evaluate integer arithmetic with parentheses and `+ - * /` |
| Wallpaper | Click a local BMP thumbnail or select with `W`/`S` and press `Enter` | Preview and persist the selected wallpaper |
| Settings | Click Appearance / Wallpaper or use `T` / `W` | Toggle and persist theme; launch wallpaper picker |
| Clock | `Esc` / header control | Return home |
| Files | `W` / `S`, `Enter`, `Backspace` | Browse folders, preview files, and go up one level |
| Launcher | `W` / `S`, `Enter` | Select and open a built-in app |

Termux:X11 / desktop touch gestures depend on the QEMU display backend forwarding touch as pointer events. The UI runner uses a tablet for absolute touch/clicks and adds a mouse device for relative movement and wheel events when QEMU provides it. If that optional device is unavailable or the host does not forward gestures, use a mouse/trackpad or the keyboard. The command prompt remains backed by the kernel shell dispatcher; it is not a separate limited parser.

## `.rpk` packages

The build installs these manifest files under `/apps` in the MyFS disk image:

- `/apps/terminal.rpk`
- `/apps/calculator.rpk`
- `/apps/clock.rpk`
- `/apps/files.rpk`
- `/apps/launcher.rpk`
- `/apps/wallpaper.rpk`
- `/apps/pengaturan.rpk`

The guest filesystem has `/wallpapers`, `/icons`, and `/settings` directories. Wallpaper images and app icons use uncompressed 24-bit or 32-bit BMP (up to 1 MiB per file); package manifests point to icons with an `icon=/icons/...bmp` line. The image decoder has bounded dimension/offset checks and rejects unsupported/compressed or truncated files. The UI uses a compact Inter-derived antialiased glyph table, which is closer to a modern Android sans-serif; it is not Xiaomi's proprietary MiSans font or Google's system Roboto package. Each `.rpk` currently uses the `RIOPKG/1` text manifest format and declares an id, display name, built-in type, entry identifier, version, category, and description. The kernel validates the manifest before opening the matching built-in app. The manifest is **not** a general executable package: applications are statically linked into the kernel until RioOS has a stable app ABI, safe loading rules, and process-local address spaces.

## Current limitations / test status

- QEMU-window keyboard and pointer input use VirtIO input devices added by `run-ui.sh` / `run-ui.ps1`; arbitrary physical input devices are not directly supported by the guest kernel.
- The UI's VirtIO input driver is polled and does not enable the Generic Timer IRQ. UART input remains a fallback.
- GUI display requires a QEMU build and display backend that support `ramfb`, the `fw_cfg` DMA interface, and the VirtIO keyboard/tablet device models. The launcher checks these models before starting.
- App manifests are metadata for built-in apps, not dynamically loadable binaries.
- The timer IRQ remains disabled by default. The UI uses polled time and does not enable periodic timer interrupts.
- Host framebuffer and UI composition tests, clean build, host regressions, and release static checks must pass before release. A QEMU visual and interactive run is a separate required acceptance test; do not call the UI bug-free until that test is performed on the target environment.

## QEMU window controls and terminal command parity

`run-ui.sh` attaches QEMU's VirtIO keyboard and tablet to the ARM `virt` machine. The UI polls these devices without enabling timer interrupts: swipe upward on the lock screen to unlock, click a home-screen app icon to open it, and type normally in the Terminal app. Serial input remains available as a fallback, so existing `WASD`/Enter workflows are retained.

The Terminal `.rpk` uses the same `execute_command()` dispatcher as the serial shell, with command output captured into the pixel terminal. This keeps command availability, filesystem changes, environment expansion, pipes, and redirection on the shared RioOS implementation rather than maintaining a second partial command list. `Esc` returns from the Terminal app to the home screen. Commands such as `exit` and `logout` are dispatched normally through the shared shell implementation, preserving their serial-shell semantics.

QEMU builds need the `virtio-keyboard-device` and `virtio-tablet-device` device models enabled. On Windows, `run-ui.ps1` attaches the same devices.


## Plain black text-console runner

`run-terminal.sh` opens QEMU’s virtual serial console as a black, monospace text window. It attaches the original RioOS UART shell to `-serial vc:100Cx32C`, but intentionally omits `ramfb`, VirtIO GUI input devices, and the `opt/rioos/start-terminal=1` flag. As a result, the pixel UI and its Terminal `.rpk` app do not launch; shell output and keyboard input are handled by the original serial shell inside the QEMU window. Use `run-ui.sh` when you want the lock/home UI and built-in apps, including the pixel Terminal. Use `run.sh` when you specifically want UART shell I/O in the host terminal. On Windows PowerShell, `run-terminal.ps1` provides the same text-console mode.
