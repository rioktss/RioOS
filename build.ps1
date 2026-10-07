$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

$VerboseBuild = $false
$Clean = $false
$RunAfterBuild = $false

foreach ($arg in $args) {
    switch ($arg) {
        "-v" { $VerboseBuild = $true }
        "--verbose" { $VerboseBuild = $true }
        "--clean" { $Clean = $true }
        "--run" { $RunAfterBuild = $true }
        "-h" {
            Write-Host "Usage: .\build.ps1 [--clean] [--verbose] [--run]"
            exit 0
        }
        "--help" {
            Write-Host "Usage: .\build.ps1 [--clean] [--verbose] [--run]"
            exit 0
        }
        default { throw "Unknown argument: $arg" }
    }
}

function Require-Command([string]$name) {
    if (-not (Get-Command $name -ErrorAction SilentlyContinue)) {
        throw "Required command not found: $name"
    }
}

function Invoke-Tool {
    param(
        [string]$Exe,
        [string[]]$Args
    )
    if ($VerboseBuild) {
        Write-Host "  -> $Exe $($Args -join ' ')"
    }
    & $Exe @Args
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed ($LASTEXITCODE): $Exe"
    }
}

Write-Host ""
Write-Host "=================================="
Write-Host "       RioOS Portable Build v2.0"
Write-Host "=================================="
Write-Host ""

$Clang = if ($env:CLANG) { $env:CLANG } else { "clang" }
$Cxx = if ($env:CXX) { $env:CXX } else { "clang++" }
$Ld = if ($env:LD) { $env:LD } else { "ld.lld" }
$ReadElf = if ($env:READELF) { $env:READELF } else { "llvm-readelf" }
$Python = if ($env:PYTHON) { $env:PYTHON } else { $null }

Require-Command $Clang
Require-Command $Cxx
Require-Command $Ld

if (-not $Python) {
    if (Get-Command python -ErrorAction SilentlyContinue) {
        $Python = "python"
    } elseif (Get-Command py -ErrorAction SilentlyContinue) {
        $Python = "py"
    } else {
        throw "Python 3 is required for tests/verify_release.py"
    }
}

New-Item -ItemType Directory -Force build, storage, "storage/home" | Out-Null

$HostTools = if ($env:MYKERNEL_TOOLS_DIR) {
    $env:MYKERNEL_TOOLS_DIR
} else {
    Join-Path $HOME ".mykernel-tools"
}
New-Item -ItemType Directory -Force $HostTools | Out-Null

if ($Clean) {
    Get-ChildItem build -File -Filter *.o -ErrorAction SilentlyContinue | Remove-Item -Force
    Remove-Item build/kernel.elf, build/user_init.elf -Force -ErrorAction SilentlyContinue
    foreach ($name in @("fs_host_test","net_host_test","mem_host_test","syscall_host_test","diag_host_test")) {
        Remove-Item (Join-Path $HostTools $name), (Join-Path $HostTools "$name.exe") -Force -ErrorAction SilentlyContinue
    }
}

function Source-Of([string]$name) {
    foreach ($dir in @("kernel","drivers","memory","process","fs","net")) {
        $p = Join-Path $dir "$name.cpp"
        if (Test-Path $p) { return $p }
    }
    throw "Source for $name not found"
}

$CxxFlags = @(
    "-Iinclude",
    "--target=aarch64-none-elf", "-mcpu=cortex-a72", "-mgeneral-regs-only", "-mstrict-align",
    "-ffreestanding", "-fno-exceptions", "-fno-rtti", "-fno-stack-protector", "-fno-builtin",
    "-fno-pic", "-fno-pie", "-fno-vectorize", "-fno-slp-vectorize",
    "-fno-unwind-tables", "-fno-asynchronous-unwind-tables",
    "-nostdinc++", "-std=c++17", "-O1", "-Wall", "-Wextra", "-Werror"
)
$AsmFlags = @(
    "-Iinclude",
    "--target=aarch64-none-elf", "-mcpu=cortex-a72",
    "-ffreestanding", "-fno-pic", "-fno-pie"
)

$Modules = @(
    "uart","memory","string","fs","vfs","process","scheduler","timer","interrupt","keyboard",
    "commands","shell","exceptions","syscall","user","mmu","virtio_mmio","virtio_blk","storage",
    "elf","virtio_net","netstack","page_alloc","fault","diag","kernel"
)

Write-Host "[1/9] Bundling userspace"
Invoke-Tool $Clang ($AsmFlags + @("-c","userspace/user_init.S","-o","build/user_init.o"))
Invoke-Tool $Ld @("-T","userspace/user_init.ld","-nostdlib","-z","max-page-size=0x1000","build/user_init.o","-o","build/user_init.elf")

Write-Host "[2/9] Building host tools"
Invoke-Tool $Cxx @("-std=c++17","-O2","-Wall","-Wextra","-pedantic","fs/mkfs_myfs.cpp","-o",(Join-Path $HostTools "mkfs_myfs.exe"))

Write-Host "[3/9] Preparing disk image"
$DiskPath = Join-Path (Get-Location) "storage/disk.img"
if (-not (Test-Path $DiskPath)) {
    $fs = [System.IO.File]::Create($DiskPath)
    $fs.SetLength(16MB)
    $fs.Dispose()
}
Invoke-Tool (Join-Path $HostTools "mkfs_myfs.exe") @("storage/disk.img","build/user_init.elf")

Write-Host "[4/9] Compiling boot"
Invoke-Tool $Clang ($AsmFlags + @("-c","boot/boot.S","-o","build/boot.o"))
Invoke-Tool $Clang ($AsmFlags + @("-c","boot/exceptions.S","-o","build/exception_vectors.o"))
Invoke-Tool $Clang ($AsmFlags + @("-c","process/user_entry.S","-o","build/user_entry.o"))

Write-Host "[5/9] Compiling kernel ($($Modules.Count) modules)"
foreach ($f in $Modules) {
    if ($VerboseBuild) { Write-Host "  -> $f.cpp" } else { Write-Host "." -NoNewline }
    $src = Source-Of $f
    Invoke-Tool $Cxx ($CxxFlags + @("-c",$src,"-o","build/$f.o"))
}
if (-not $VerboseBuild) { Write-Host " done" }

Write-Host "[6/9] Linking"
$linkObjects = @(
    "build/boot.o","build/exception_vectors.o","build/user_entry.o",
    "build/kernel.o","build/uart.o","build/memory.o","build/string.o","build/fs.o","build/vfs.o",
    "build/process.o","build/scheduler.o","build/timer.o","build/interrupt.o","build/keyboard.o",
    "build/commands.o","build/shell.o","build/exceptions.o","build/syscall.o","build/user.o","build/mmu.o",
    "build/virtio_mmio.o","build/virtio_blk.o","build/storage.o","build/elf.o","build/virtio_net.o",
    "build/netstack.o","build/page_alloc.o","build/fault.o","build/diag.o"
)
Invoke-Tool $Ld (@("-T","linker.ld","-nostdlib","-z","max-page-size=0x1000") + $linkObjects + @("-o","build/kernel.elf"))

Write-Host "[7/9] Verifying ELF"
if (Get-Command $ReadElf -ErrorAction SilentlyContinue) {
    & $ReadElf -h build/kernel.elf | Select-String "Class:|Machine:|Entry point"
}

Write-Host "[8/9] Running host tests"
Invoke-Tool $Cxx @("-std=c++17","-O2","-Wall","-Wextra","-pedantic","-iquote","include","-DHOST_TEST","tests/fs_host_test.cpp","fs/fs.cpp","kernel/string.cpp","-o",(Join-Path $HostTools "fs_host_test.exe"))
& (Join-Path $HostTools "fs_host_test.exe")
if ($LASTEXITCODE -ne 0) { throw "FS host test failed" }

Invoke-Tool $Cxx @("-std=c++17","-O2","-Wall","-Wextra","-Werror","-pedantic","-iquote","include","-DHOST_TEST","tests/net_host_test.cpp","net/netstack.cpp","-o",(Join-Path $HostTools "net_host_test.exe"))
& (Join-Path $HostTools "net_host_test.exe")
if ($LASTEXITCODE -ne 0) { throw "NET host test failed" }

Invoke-Tool $Cxx @("-std=c++17","-O2","-Wall","-Wextra","-pedantic","-iquote","include","-DHOST_TEST","tests/mem_host_test.cpp","memory/page_alloc.cpp","kernel/fault.cpp","-o",(Join-Path $HostTools "mem_host_test.exe"))
& (Join-Path $HostTools "mem_host_test.exe")
if ($LASTEXITCODE -ne 0) { throw "MEM host test failed" }

Invoke-Tool $Cxx @("-std=c++17","-O2","-Wall","-Wextra","-pedantic","-iquote","include","-DHOST_TEST","tests/syscall_host_test.cpp","kernel/syscall.cpp","-o",(Join-Path $HostTools "syscall_host_test.exe"))
& (Join-Path $HostTools "syscall_host_test.exe")
if ($LASTEXITCODE -ne 0) { throw "SYSCALL host test failed" }

Invoke-Tool $Cxx @("-std=c++17","-O2","-Wall","-Wextra","-pedantic","-iquote","include","-DHOST_TEST","tests/diag_host_test.cpp","kernel/diag.cpp","-o",(Join-Path $HostTools "diag_host_test.exe"))
& (Join-Path $HostTools "diag_host_test.exe")
if ($LASTEXITCODE -ne 0) { throw "DIAG host test failed" }

Write-Host "[9/9] Release verification"
if ($Python -eq "py") {
    & py -3 tests/verify_release.py
} else {
    & $Python tests/verify_release.py
}
if ($LASTEXITCODE -ne 0) { throw "Release verification failed" }

Write-Host ""
Write-Host "=================================="
Write-Host "          BUILD SUCCESS (v2.0)"
Write-Host "=================================="
Write-Host ""
Write-Host "  Kernel: build/kernel.elf (AArch64)"
Write-Host "  Disk  : storage/disk.img"
Write-Host "  Tests : FS PASS | NET PASS | MEM PASS | SYSCALL PASS | DIAG PASS"
Write-Host ""

if ($RunAfterBuild) {
    & (Join-Path $PSScriptRoot "run.ps1")
}
