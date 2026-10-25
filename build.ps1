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
        "-h" { Write-Host "Usage: .\build.ps1 [--clean] [--verbose] [--run]"; exit 0 }
        "--help" { Write-Host "Usage: .\build.ps1 [--clean] [--verbose] [--run]"; exit 0 }
        default { throw "Unknown argument: $arg" }
    }
}

function Require-Command([string]$name) {
    if (-not (Get-Command $name -ErrorAction SilentlyContinue)) {
        throw "Required command not found: $name"
    }
}
function Invoke-Tool {
    param([string]$Exe, [string[]]$ToolArgs)
    if ($VerboseBuild) { Write-Host "  -> $Exe $($ToolArgs -join ' ')" }
    & $Exe @ToolArgs
    if ($LASTEXITCODE -ne 0) { throw "Command failed ($LASTEXITCODE): $Exe" }
}

Write-Host ""
Write-Host "=================================="
Write-Host "       RioOS Pixel UI Upgrade v1.11.0"
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
    if (Get-Command python3 -ErrorAction SilentlyContinue) { $Python = "python3" }
    elseif (Get-Command python -ErrorAction SilentlyContinue) { $Python = "python" }
    elseif (Get-Command py -ErrorAction SilentlyContinue) { $Python = "py" }
    else { throw "Python 3 is required for tests/verify_release.py" }
}

New-Item -ItemType Directory -Force build, storage, "storage/home" | Out-Null
$HostTools = if ($env:MYKERNEL_TOOLS_DIR) { $env:MYKERNEL_TOOLS_DIR } else { Join-Path $HOME ".mykernel-tools" }
New-Item -ItemType Directory -Force $HostTools | Out-Null

if ($Clean) {
    Get-ChildItem build -File -Filter *.o -ErrorAction SilentlyContinue | Remove-Item -Force
    foreach ($artifact in @("kernel.elf","user_init.elf","nano.elf","calc.elf")) {
        Remove-Item (Join-Path "build" $artifact) -Force -ErrorAction SilentlyContinue
    }
    foreach ($name in @("fs_host_test","net_host_test","mem_host_test","syscall_host_test","diag_host_test","vfs_host_test","sched_host_test","env_host_test","fd_host_test","process_host_test","scheduler_process_host_test","smp_host_test","fb_host_test","ui_host_test")) {
        Remove-Item (Join-Path $HostTools $name), (Join-Path $HostTools "$name.exe") -Force -ErrorAction SilentlyContinue
    }
}

function Source-Of([string]$name) {
    $uiFiles = @{
        ui_boot = "ui/boot.cpp"; ui_lock_screen = "ui/lock_screen.cpp";
        ui_home_screen = "ui/home_screen.cpp"; ui_apps = "ui/apps.cpp";
        ui_terminal = "ui/terminal.cpp"; ui_calculator = "ui/calculator.cpp";
        ui_clock = "ui/clock.cpp"; ui_file_manager = "ui/file_manager.cpp";
        ui_launcher = "ui/launcher.cpp"; ui_core = "ui/ui.cpp"
    }
    if ($uiFiles.ContainsKey($name)) { return $uiFiles[$name] }
    foreach ($dir in @("kernel","drivers","memory","process","fs","net","ui")) {
        $p = Join-Path $dir "$name.cpp"
        if (Test-Path $p) { return $p }
    }
    throw "Source for $name not found"
}

$CxxFlags = @(
    "-Iinclude", "--target=aarch64-none-elf", "-mcpu=cortex-a72", "-mgeneral-regs-only", "-mstrict-align",
    "-ffreestanding", "-fno-exceptions", "-fno-rtti", "-fno-stack-protector", "-fno-builtin",
    "-fno-pic", "-fno-pie", "-fno-vectorize", "-fno-slp-vectorize",
    "-fno-unwind-tables", "-fno-asynchronous-unwind-tables", "-nostdinc++", "-std=c++17", "-O1",
    "-Wall", "-Wextra", "-Werror"
)
$AsmFlags = @("-Iinclude", "--target=aarch64-none-elf", "-mcpu=cortex-a72", "-ffreestanding", "-fno-pic", "-fno-pie")
$Modules = @(
    "uart","memory","string","fs","vfs","process","sched_policy","scheduler","timer","interrupt","keyboard",
    "commands","version","shell","history","env","rtc","exceptions","syscall","user","mmu","virtio_mmio","virtio_input","virtio_blk",
    "storage","thread","fd","fb","font","elf","virtio_net","netstack","page_alloc","fault","diag","security","smp",
    "ui_boot","ui_lock_screen","ui_home_screen","ui_apps","ui_terminal","ui_calculator","ui_clock","ui_file_manager","ui_launcher","ui_core","kernel"
)

Write-Host "[1/9] Bundling userspace (init + nano + calc)"
Invoke-Tool $Clang ($AsmFlags + @("-c","userspace/user_init.S","-o","build/user_init.o"))
Invoke-Tool $Ld @("-T","userspace/user_init.ld","-nostdlib","-z","max-page-size=0x1000","build/user_init.o","-o","build/user_init.elf")
if (Test-Path userspace/nano.S) {
    Invoke-Tool $Clang ($AsmFlags + @("-c","userspace/nano.S","-o","build/nano.o"))
    Invoke-Tool $Ld @("-T","userspace/user_init.ld","-nostdlib","-z","max-page-size=0x1000","build/nano.o","-o","build/nano.elf")
} else { Copy-Item build/user_init.elf build/nano.elf -Force }
if (Test-Path userspace/calc.cpp) {
    $CalcFlags = @("-Iinclude","--target=aarch64-none-elf","-mcpu=cortex-a72","-ffreestanding","-fno-exceptions","-fno-rtti","-fno-stack-protector","-fno-builtin","-fno-pic","-fno-pie","-fno-vectorize","-fno-slp-vectorize","-fno-unwind-tables","-fno-asynchronous-unwind-tables","-nostdinc++","-nostdlib","-std=c++17","-O1","-Wall","-Wextra","-Werror")
    Invoke-Tool $Cxx ($CalcFlags + @("-c","userspace/calc.cpp","-o","build/calc.o"))
    Invoke-Tool $Ld @("-T","userspace/user_init.ld","-nostdlib","-z","max-page-size=0x1000","build/calc.o","-o","build/calc.elf")
}

Write-Host "[2/9] Building host tools"
Invoke-Tool $Cxx @("-std=c++17","-O2","-Wall","-Wextra","-pedantic","fs/mkfs_myfs.cpp","-o",(Join-Path $HostTools "mkfs_myfs.exe"))

Write-Host "[3/9] Preparing disk image"
$DiskPath = Join-Path (Get-Location) "storage/disk.img"
if (-not (Test-Path $DiskPath)) {
    $fs = [System.IO.File]::Create($DiskPath)
    $fs.SetLength(32768L * 512L)
    $fs.Dispose()
}
Invoke-Tool (Join-Path $HostTools "mkfs_myfs.exe") @("storage/disk.img","build/user_init.elf","build/nano.elf","build/calc.elf","apps/terminal.rpk","apps/calculator.rpk","apps/clock.rpk","apps/files.rpk","apps/launcher.rpk")

Write-Host "[4/9] Compiling boot and context-switch assembly"
Invoke-Tool $Clang ($AsmFlags + @("-c","boot/boot.S","-o","build/boot.o"))
Invoke-Tool $Clang ($AsmFlags + @("-c","boot/exceptions.S","-o","build/exception_vectors.o"))
Invoke-Tool $Clang ($AsmFlags + @("-c","process/user_entry.S","-o","build/user_entry.o"))
Invoke-Tool $Clang ($AsmFlags + @("-c","process/ctx_switch.S","-o","build/ctx_switch.o"))

Write-Host "[5/9] Compiling kernel ($($Modules.Count) modules)"
foreach ($f in $Modules) {
    if ($VerboseBuild) { Write-Host "  -> $f.cpp" } else { Write-Host "." -NoNewline }
    $src = Source-Of $f
    Invoke-Tool $Cxx ($CxxFlags + @("-c",$src,"-o","build/$f.o"))
}
if (-not $VerboseBuild) { Write-Host " done" }

Write-Host "[6/9] Linking"
$linkObjects = @(
    "build/boot.o","build/exception_vectors.o","build/user_entry.o","build/ctx_switch.o",
    "build/kernel.o","build/uart.o","build/memory.o","build/string.o","build/fs.o","build/vfs.o",
    "build/process.o","build/sched_policy.o","build/scheduler.o","build/timer.o","build/interrupt.o","build/keyboard.o",
    "build/commands.o","build/version.o","build/shell.o","build/history.o","build/env.o","build/rtc.o","build/exceptions.o",
    "build/syscall.o","build/user.o","build/mmu.o","build/virtio_mmio.o","build/virtio_input.o","build/virtio_blk.o","build/storage.o","build/elf.o",
    "build/virtio_net.o","build/netstack.o","build/page_alloc.o","build/fault.o","build/diag.o","build/security.o",
    "build/fb.o","build/font.o","build/thread.o","build/fd.o","build/smp.o",
    "build/ui_boot.o","build/ui_lock_screen.o","build/ui_home_screen.o","build/ui_apps.o",
    "build/ui_terminal.o","build/ui_calculator.o","build/ui_clock.o","build/ui_file_manager.o","build/ui_launcher.o","build/ui_core.o"
)
Invoke-Tool $Ld (@("-T","linker.ld","-nostdlib","-z","max-page-size=0x1000") + $linkObjects + @("-o","build/kernel.elf"))

Write-Host "[7/9] Verifying ELF"
if (Get-Command $ReadElf -ErrorAction SilentlyContinue) {
    & $ReadElf -h build/kernel.elf | Select-String "Class:|Machine:|Entry point"
}

function Invoke-HostTest([string]$name, [string[]]$sources, [bool]$strict = $false) {
    $exe = Join-Path $HostTools "$name.exe"
    $flags = @("-std=c++17","-O2","-Wall","-Wextra","-pedantic","-iquote","include","-DHOST_TEST")
    if ($strict) { $flags += "-Werror" }
    Invoke-Tool $Cxx ($flags + $sources + @("-o",$exe))
    & $exe
    if ($LASTEXITCODE -ne 0) { throw "$name failed" }
}
Write-Host "[8/9] Running 14 host tests plus pixel UI regressions"
Invoke-HostTest "fs_host_test" @("tests/fs_host_test.cpp","fs/fs.cpp","kernel/string.cpp","kernel/rtc.cpp")
Invoke-HostTest "net_host_test" @("tests/net_host_test.cpp","net/netstack.cpp") $true
Invoke-HostTest "mem_host_test" @("tests/mem_host_test.cpp","memory/page_alloc.cpp","kernel/fault.cpp")
Invoke-HostTest "syscall_host_test" @("tests/syscall_host_test.cpp","kernel/syscall.cpp","kernel/fd.cpp")
Invoke-HostTest "diag_host_test" @("tests/diag_host_test.cpp","kernel/diag.cpp")
Invoke-HostTest "vfs_host_test" @("tests/vfs_host_test.cpp","fs/vfs.cpp","fs/fs.cpp","kernel/string.cpp","kernel/rtc.cpp") $true
Invoke-HostTest "sched_host_test" @("tests/sched_host_test.cpp","process/sched_policy.cpp") $true
Invoke-HostTest "env_host_test" @("tests/env_host_test.cpp","kernel/env.cpp","kernel/string.cpp") $true
Invoke-HostTest "fd_host_test" @("tests/fd_host_test.cpp","kernel/fd.cpp","fs/fs.cpp","kernel/string.cpp","kernel/rtc.cpp") $true
Invoke-HostTest "process_host_test" @("tests/process_host_test.cpp","process/process.cpp","kernel/string.cpp") $true
Invoke-HostTest "scheduler_process_host_test" @("tests/scheduler_process_host_test.cpp","process/scheduler.cpp","process/process.cpp","kernel/string.cpp") $true
Invoke-HostTest "smp_host_test" @("tests/smp_host_test.cpp","kernel/smp.cpp") $true
Invoke-HostTest "fb_host_test" @("tests/fb_host_test.cpp","drivers/fb.cpp","drivers/font.cpp") $true
Invoke-HostTest "ui_host_test" @("tests/ui_host_test.cpp","drivers/fb.cpp","drivers/font.cpp","ui/boot.cpp","ui/lock_screen.cpp","ui/home_screen.cpp") $true

Write-Host "[9/9] Release verification"
if ($Python -eq "py") { & py -3 tests/verify_release.py } else { & $Python tests/verify_release.py }
if ($LASTEXITCODE -ne 0) { throw "Release verification failed" }

Write-Host ""
Write-Host "=================================="
Write-Host "       BUILD SUCCESS (v1.11.0)"
Write-Host "=================================="
Write-Host "  Kernel: build/kernel.elf (AArch64)"
Write-Host "  Disk  : storage/disk.img"
Write-Host "  Tests : 14 host tests passed"
Write-Host "  Pixel UI : QEMU ramfb backend (run-ui.ps1)"
Write-Host "  RPK apps : terminal, calculator, clock, files, launcher"
Write-Host "  Timer IRQ: remains disabled during normal boot"
Write-Host ""
if ($RunAfterBuild) { & (Join-Path $PSScriptRoot "run.ps1") }
