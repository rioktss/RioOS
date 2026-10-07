$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

$Qemu = if ($env:QEMU) { $env:QEMU } else { "qemu-system-aarch64" }
$Kernel = if ($env:KERNEL) { $env:KERNEL } else { "build/kernel.elf" }
$Disk = if ($env:DISK) { $env:DISK } else { "storage/disk.img" }
$Mem = if ($env:RIOOS_MEM) { $env:RIOOS_MEM } else { "128M" }
$Cpu = if ($env:RIOOS_CPU) { $env:RIOOS_CPU } else { "cortex-a72" }

if (-not (Get-Command $Qemu -ErrorAction SilentlyContinue)) {
    throw "qemu-system-aarch64 not found"
}
if (-not (Test-Path $Kernel)) {
    throw "$Kernel not found. Run .\build.ps1 first."
}
if (-not (Test-Path $Disk)) {
    throw "$Disk not found. Run .\build.ps1 first."
}

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
    "-netdev" "user,id=n1,ipv4=on,ipv6=off" `
    "-device" "virtio-net-device,netdev=n1"

if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}
