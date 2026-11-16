$ErrorActionPreference = "Stop"
$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
Set-Location $ProjectRoot

$Qemu = if ($env:QEMU) { $env:QEMU } else { "qemu-system-aarch64" }
$Kernel = if ($env:KERNEL) { $env:KERNEL } else { "build/kernel.elf" }
$Disk = if ($env:DISK) { $env:DISK } else { "storage/disk.img" }
$Mem = if ($env:RIOOS_MEM) { $env:RIOOS_MEM } else { "128M" }
$Cpu = if ($env:RIOOS_CPU) { $env:RIOOS_CPU } else { "cortex-a72" }

if (-not (Get-Command $Qemu -ErrorAction SilentlyContinue)) {
    throw "qemu-system-aarch64 not found"
}
if (-not (Test-Path $Kernel)) {
    throw "$Kernel not found. Run .\windows\build.ps1 first."
}
if (-not (Test-Path $Disk)) {
    throw "$Disk not found. Run .\windows\build.ps1 first."
}

# Optional extra devices (all off by default):
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

& $Qemu `
    "-M" "virt,gic-version=2,virtualization=on" `
    "-global" "virtio-mmio.force-legacy=false" `
    "-cpu" $Cpu `
    "-m" $Mem `
    "-nographic" `
    "-monitor" "none" `
    "-kernel" $Kernel `
    "-drive" "if=none,format=raw,file=$Disk,id=vd0" `
    "-device" "virtio-blk-device,drive=vd0" `
    "-device" "virtio-rng-device" `
    "-netdev" "user,id=n1,ipv4=on,ipv6=off" `
    "-device" "virtio-net-device,netdev=n1" `
    @Extra

if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}
