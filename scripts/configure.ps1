# KillEngine — Script de configuration CMake
# Usage: .\scripts\configure.ps1

$ErrorActionPreference = "Stop"

# Qt path
$QT_PATH = "C:\Qt\6.8.1\msvc2022_64"

if (-not (Test-Path $QT_PATH)) {
    Write-Host "ERROR: Qt 6.8.1 not found at $QT_PATH" -ForegroundColor Red
    Write-Host "Install it with: python -m aqt install-qt windows desktop 6.8.1 win64_msvc2022_64 -m qtwebengine -O C:\Qt"
    exit 1
}

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

Write-Host "Using MSVC: $vcvars" -ForegroundColor Green
Write-Host "Using Qt: $QT_PATH" -ForegroundColor Green

# Get ninja from pip install
$ninjaPath = (python -c "import ninja; print(ninja.BIN_DIR)" 2>$null)
if (-not $ninjaPath) {
    Write-Host "Installing Ninja..." -ForegroundColor Yellow
    pip install ninja 2>$null
    $ninjaPath = (python -c "import ninja; print(ninja.BIN_DIR)")
}

# Configure with CMake using vcvars environment
$batchContent = @"
@echo off
call "$vcvars" >nul 2>&1
set "PATH=$ninjaPath;%PATH%"
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DCMAKE_PREFIX_PATH="$QT_PATH" -DKILLENGINE_BUILD_TESTS=ON
"@

$tempBat = Join-Path $env:TEMP "killengine_configure.bat"
Set-Content -Path $tempBat -Value $batchContent -Encoding ASCII
& cmd /c $tempBat
Remove-Item $tempBat -ErrorAction SilentlyContinue

Write-Host "`nConfiguration complete!" -ForegroundColor Green
Write-Host "Run .\scripts\build.ps1 to build." -ForegroundColor Cyan
