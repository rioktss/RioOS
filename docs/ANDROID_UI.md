# RioOS Android-like UI

RioOS ships a small bare-metal UI for QEMU `virt` and `ramfb`. It uses native framebuffer drawing and built-in C++ modules; no desktop graphics library is linked into the kernel. UI labels use a compact, antialiased proportional glyph table derived from Inter; the serial console retains its original fixed-cell font. The styling is inspired by Android/MIUI rather than a pixel-art desktop.

## Features

- Boot splash, lock screen, four-column Home grid, status bar, and dock.
- Swipe down from the top edge (or press `C`) to open Control Centre.
- Quick actions for network status, dark/light theme, Wallpaper, Settings, locking, and closing the panel.
- `wallpaper.rpk` selects local images from `/wallpapers`; the choice is saved in `/settings/wallpaper.cfg`.
- `pengaturan.rpk` opens appearance settings and stores the selected theme in `/settings/theme.cfg`.
- RPK manifests can reference local icon images with `icon=/icons/<name>.bmp`.
- Static screens are not fully repainted every second. Clock pages refresh once per minute, reducing framebuffer churn and visible flicker. Timer IRQ remains off.

## Local wallpaper files

Supported format is uncompressed BMP (24-bit or 32-bit), maximum file size **1 MiB**. To bundle additional wallpapers, copy BMP files into `assets/wallpapers/` and rebuild:

```sh
cp ~/Pictures/my-wallpaper.bmp assets/wallpapers/
bash build.sh
```

Open RioOS, launch Wallpaper, select the image, and press Enter. The selected path is persisted on the filesystem. The bundled `rioos-default.bmp` is used if no saved choice is available or a saved image cannot be decoded.

For best results, prepare a landscape image around 640×480 or smaller. Large phone photos may exceed the 1 MiB filesystem file limit; resize them before copying. The decoder rejects compressed BMP, PNG, JPEG, unsupported bit depths, malformed dimensions, and truncated files safely.

## Local application icons

Replace the BMP file in `assets/icons/` matching an app ID, then rebuild. For example, `assets/icons/terminal.bmp` is referenced by `apps/terminal.rpk` as `/icons/terminal.bmp`. Icons are drawn with rounded corners and the old geometric icon remains as a fallback if a local icon file is missing or malformed.

The app packages are metadata manifests (`RIOPKG/1`) for built-in kernel UI modules. They are not yet general-purpose dynamically loaded third-party binary apps; the `entry` field selects a built-in handler.

## Input and launch

Use `bash build.sh` to compile and run host tests. Start the interactive QEMU window from a graphical X11/Wayland desktop with `bash run-ui.sh`. Swipe up on the lock screen or press a key to unlock. Click an app, use the keyboard for in-app commands, press `Q` / Escape to go back, and click the small top-left area for a global Home shortcut.

Inter font licensing and attribution are included in `docs/THIRD_PARTY_NOTICES.md`; the original font file is not bundled. This is a best-effort improvement, not a claim that all hardware/backends are bug-free. Host tests cover drawing primitives, manifest checks, filesystem bounds, and release invariants; real QEMU X11 input/rendering should still be checked on the target machine.
