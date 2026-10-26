$ErrorActionPreference = "Stop"
$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
Set-Location $ProjectRoot
$Qemu = if ($env:QEMU) { $env:QEMU } else { "qemu-system-aarch64" }
$Mem = if ($env:RIOOS_MEM) { $env:RIOOS_MEM } else { "128M" }
$Cpu = if ($env:RIOOS_CPU) { $env:RIOOS_CPU } else { "cortex-a72" }
$Display = if ($env:RIOOS_DISPLAY) { $env:RIOOS_DISPLAY } else { "sdl" }
if (-not (Get-Command $Qemu -ErrorAction SilentlyContinue)) {
    throw "qemu-system-aarch64 not found. Install a QEMU build with SDL/GTK display support."
}
if (-not (Test-Path "build/kernel.elf") -or -not (Test-Path "storage/disk.img")) {
    throw "Build artifacts missing. Run .\windows\build.ps1 first."
}
$DeviceHelp = (& $Qemu -device help 2>&1 | Out-String)
foreach ($RequiredDevice in @("ramfb", "virtio-keyboard-device", "virtio-tablet-device")) {
    if (-not $DeviceHelp.Contains("name `"$RequiredDevice`"")) {
        throw "This QEMU build lacks '$RequiredDevice'. Install a QEMU system-aarch64 build with ramfb and VirtIO input devices."
    }
}
$MouseDeviceArgs = @()
if ($DeviceHelp.Contains('name "virtio-mouse-device"')) {
    $MouseDeviceArgs = @("-device", "virtio-mouse-device")
} else {
    Write-Warning "virtio-mouse-device is unavailable; wheel scrolling is disabled, but tablet input remains enabled."
}
Write-Host "RioOS UI: keyboard and tablet input, with mouse-wheel scrolling when QEMU supports it."
Write-Host "Serial keyboard remains available as a fallback."
& $Qemu `
    -M "virt,gic-version=2,virtualization=on" `
    -global "virtio-mmio.force-legacy=false" `
    -cpu $Cpu `
    -m $Mem `
    -display $Display `
    -serial stdio `
    -monitor none `
    -kernel "build/kernel.elf" `
    -device ramfb `
    -drive "if=none,format=raw,file=storage/disk.img,id=vd0" `
    -device "virtio-blk-device,drive=vd0" `
    -netdev "user,id=n1,ipv4=on,ipv6=off" `
    -device "virtio-net-device,netdev=n1" `
    -device "virtio-keyboard-device" `
    -device "virtio-tablet-device" `
    @MouseDeviceArgs
if ($LASTEXITCODE -ne 0) { throw "QEMU UI session failed ($LASTEXITCODE)" }
