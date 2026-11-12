# Build RioOS di Windows: delegasi ke linux/build.sh (satu sumber kebenaran).
# Butuh bash (Git Bash atau WSL) + clang/lld/python3 di PATH.
$ErrorActionPreference = "Stop"
$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
Set-Location $ProjectRoot
$bash = Get-Command bash -ErrorAction SilentlyContinue
if (-not $bash) { throw "bash tidak ditemukan. Install Git for Windows (Git Bash) atau WSL." }
$RunAfterBuild = $args -contains "--run"
$rest = @($args | Where-Object { $_ -ne "--run" })
& $bash.Source "linux/build.sh" @rest
if ($LASTEXITCODE -ne 0) { throw "Build gagal ($LASTEXITCODE)" }
if ($RunAfterBuild) { & (Join-Path $PSScriptRoot "run.ps1") }
