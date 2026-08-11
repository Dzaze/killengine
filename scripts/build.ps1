# KillEngine — Script de build
# Usage: .\scripts\build.ps1

$ErrorActionPreference = "Stop"

# Find Visual Studio vcvars64.bat
$vcvars = $null
@(
    "C:\Program Files\Microsoft Visual Studio\2022\*\VC\Auxiliary\Build\vcvars64.bat",
    "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat",
    "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat",
    "C:\Program Files (x86)\Microsoft Visual Studio\2019\Professional\VC\Auxiliary\Build\vcvars64.bat",
    "C:\Program Files (x86)\Microsoft Visual Studio\2019\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
) | ForEach-Object {
    $resolved = Resolve-Path $_ -ErrorAction SilentlyContinue
    if ($resolved -and -not $vcvars) { $vcvars = $resolved.Path }
}

if (-not $vcvars) {
    Write-Host "ERROR: vcvars64.bat not found!" -ForegroundColor Red
    exit 1
}

# Get ninja from pip install
$ninjaPath = (python -c "import ninja; print(ninja.BIN_DIR)" 2>$null)

Write-Host "Building KillEngine..." -ForegroundColor Cyan

$batchContent = @"
@echo off
call "$vcvars" >nul 2>&1
set "VSLANG=1033"
set "PATH=$ninjaPath;%PATH%"
cmake --build build --config Release
"@

$tempBat = Join-Path $env:TEMP "killengine_build.bat"
Set-Content -Path $tempBat -Value $batchContent -Encoding ASCII
& cmd /c $tempBat
Remove-Item $tempBat -ErrorAction SilentlyContinue

if ($LASTEXITCODE -eq 0) {
    Write-Host "`nBuild successful!" -ForegroundColor Green
    Write-Host "Executable: build\bin\KillEngine.exe" -ForegroundColor Cyan
} else {
    Write-Host "`nBuild FAILED!" -ForegroundColor Red
    exit 1
}
