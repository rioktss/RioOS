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

Write-Host "RioOS text shell: opening a plain QEMU console window ($Display)."
Write-Host "This is the original black text shell, not the pixel UI Terminal .rpk app."

# Do not add ramfb/fw_cfg/GUI input devices here; those start the pixel UI.
& $Qemu `
    -M "virt,gic-version=2,virtualization=on" `
    -global "virtio-mmio.force-legacy=false" `
    -cpu $Cpu `
    -m $Mem `
    -display $Display `
    -serial "vc:100Cx32C" `
    -monitor none `
    -kernel $Kernel `
    -drive "if=none,format=raw,file=$Disk,id=vd0" `
    -device "virtio-blk-device,drive=vd0" `
    -netdev "user,id=n1,ipv4=on,ipv6=off" `
    -device "virtio-net-device,netdev=n1"
if ($LASTEXITCODE -ne 0) { throw "QEMU text console exited with code $LASTEXITCODE" }
