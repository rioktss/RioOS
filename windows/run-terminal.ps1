# Run RioOS and show its shell in a QEMU window (framebuffer terminal).
#   run.ps1           -> RioOS in this PowerShell window (serial, -nographic)
#   run-terminal.ps1  -> RioOS in a QEMU window: shell only, mouse-wheel scrollback
$ErrorActionPreference = "Stop"
$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
Set-Location $ProjectRoot

$Qemu = if ($env:QEMU) { $env:QEMU } else { "qemu-system-aarch64" }
$Kernel = if ($env:KERNEL) { $env:KERNEL } else { "build/kernel.elf" }
$Disk = if ($env:DISK) { $env:DISK } else { "storage/disk.img" }
$Mem = if ($env:RIOOS_MEM) { $env:RIOOS_MEM } else { "128M" }
$Cpu = if ($env:RIOOS_CPU) { $env:RIOOS_CPU } else { "cortex-a72" }
$Display = if ($env:RIOOS_DISPLAY) { $env:RIOOS_DISPLAY } else { "sdl" }

if (-not (Get-Command $Qemu -ErrorAction SilentlyContinue)) {
    throw "qemu-system-aarch64 not found. Install QEMU with SDL or GTK display support."
}
if (-not (Test-Path $Kernel)) { throw "$Kernel not found. Run .\windows\build.ps1 first." }
if (-not (Test-Path $Disk)) { throw "$Disk not found. Run .\windows\build.ps1 first." }

$DisplayHelp = (& $Qemu -display help 2>&1 | Out-String)
if (-not $DisplayHelp.Contains($Display)) {
    $Fallback = $null
    foreach ($Candidate in @("sdl", "gtk")) {
        if ($DisplayHelp.Contains($Candidate)) { $Fallback = $Candidate; break }
    }
    if (-not $Fallback) { throw "This QEMU build has no SDL/GTK graphical display backend." }
    Write-Warning "Display '$Display' unavailable; using '$Fallback'."
    $Display = $Fallback
}

$FbDev = if ($env:RIOOS_GPU -eq "virtio") { "virtio-gpu-device" } else { "ramfb" }
$DeviceHelp = (& $Qemu -device help 2>&1 | Out-String)
foreach ($RequiredDevice in @($FbDev, "virtio-keyboard-device", "virtio-tablet-device")) {
    if (-not $DeviceHelp.Contains("name `"$RequiredDevice`"")) {
        throw "This QEMU build lacks '$RequiredDevice'. Install a QEMU system-aarch64 build with ramfb and VirtIO input devices."
    }
}

# Optional extra devices (all off by default):
#   $env:RIOOS_GPU = "virtio" virtio-gpu (2D) instead of ramfb
#   $env:RIOOS_USB = "1"        xHCI controller + USB keyboard + USB tablet
#   $env:RIOOS_SOUND = "dsound" virtio-sound with a host audio driver (dsound, sdl, wav ...); "1" = dsound; "wav" writes $env:RIOOS_WAV (default rioos-audio.wav)
#   virtio-9p (RIOOS_9P) needs QEMU's virtfs, which Windows builds do not have; use a Linux/macOS host or WSL for host-folder sharing.
$Extra = @()
function Test-QemuDevice([string]$Name) {
    $Help = (& $Qemu -device help 2>&1 | Out-String)
    if (-not $Help.Contains("name `"$Name`"")) { throw "This QEMU build lacks '$Name'." }
}
if ($env:RIOOS_USB -and $env:RIOOS_USB -ne "0") {
    Test-QemuDevice "qemu-xhci"; Test-QemuDevice "usb-kbd"; Test-QemuDevice "usb-tablet"
    $Extra += @("-device", "qemu-xhci,id=xhci", "-device", "usb-kbd,bus=xhci.0", "-device", "usb-tablet,bus=xhci.0")
}
if ($env:RIOOS_SOUND -and $env:RIOOS_SOUND -ne "0") {
    Test-QemuDevice "virtio-sound-device"
    $Snd = $env:RIOOS_SOUND
    if ($Snd -eq "1") { $Snd = "dsound" }
    if ($Snd -eq "wav") {
        $WavPath = if ($env:RIOOS_WAV) { $env:RIOOS_WAV } else { "rioos-audio.wav" }
        $Extra += @("-audiodev", "wav,id=snd0,path=$WavPath")
    } else {
        $Extra += @("-audiodev", "$Snd,id=snd0")
    }
    $Extra += @("-device", "virtio-sound-device,audiodev=snd0")
}
if ($env:RIOOS_9P) { throw "RIOOS_9P: virtio-9p (virtfs) is not available in QEMU for Windows." }

Write-Host "RioOS terminal window: QEMU ($Display). Type in the window; scroll with the mouse wheel."

& $Qemu `
    -M "virt,gic-version=2,virtualization=on" `
    -global "virtio-mmio.force-legacy=false" `
    -cpu $Cpu `
    -m $Mem `
    -display $Display `
    -serial stdio `
    -monitor none `
    -kernel $Kernel `
    -device $FbDev `
    -drive "if=none,format=raw,file=$Disk,id=vd0" `
    -device "virtio-blk-device,drive=vd0" `
    -device "virtio-rng-device" `
    -netdev "user,id=n1,ipv4=on,ipv6=off" `
    -device "virtio-net-device,netdev=n1" `
    -device "virtio-keyboard-device" `
    -device "virtio-tablet-device" `
    @Extra
if ($LASTEXITCODE -ne 0) { throw "QEMU terminal window exited with code $LASTEXITCODE" }
