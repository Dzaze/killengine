# Build and stage a portable Lua runtime for KillEngine.
#
# Usage:
#   .\scripts\setup-lua-runtime.ps1
#   .\scripts\setup-lua-runtime.ps1 -Version 5.5.1

param(
    [string]$Version = "5.5.1",
    [switch]$Force
)

$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$downloadsRoot = Join-Path $repoRoot "downloads"
$buildRoot = Join-Path $downloadsRoot "lua-build"
$runtimeRoot = Join-Path $repoRoot "runtime\lua"
$archiveName = "lua-$Version.tar.gz"
$archivePath = Join-Path $downloadsRoot $archiveName
$sourceUrl = "https://www.lua.org/ftp/$archiveName"

$knownSha256 = @{
    "5.5.1" = "1c4b4068d67061f2a2231ad2b5422e77acea1487ea9890f6320af614f4373dce"
    "5.4.8" = "4f18ddae154e793e46eeab727c59ef1c0c0c2b744e7b94219710d76f530629ae"
}

function Find-VcVars64 {
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
    return $vcvars
}

New-Item -ItemType Directory -Force -Path $downloadsRoot, $buildRoot, $runtimeRoot | Out-Null

if ((Test-Path -LiteralPath (Join-Path $runtimeRoot "lua.exe")) -and -not $Force) {
    Write-Host "Lua runtime already present: $(Join-Path $runtimeRoot "lua.exe")" -ForegroundColor Green
    Write-Host "Use -Force to rebuild it." -ForegroundColor Cyan
    exit 0
}

if (-not (Test-Path -LiteralPath $archivePath) -or $Force) {
    Write-Host "Downloading Lua $Version from $sourceUrl" -ForegroundColor Cyan
    Invoke-WebRequest -Uri $sourceUrl -OutFile $archivePath
}

if ($knownSha256.ContainsKey($Version)) {
    $actual = (Get-FileHash -Algorithm SHA256 -LiteralPath $archivePath).Hash.ToLowerInvariant()
    $expected = $knownSha256[$Version].ToLowerInvariant()
    if ($actual -ne $expected) {
        throw "Lua archive checksum mismatch for $archiveName. Expected $expected, got $actual."
    }
    Write-Host "SHA256 verified: $actual" -ForegroundColor Green
} else {
    Write-Warning "No pinned SHA256 for Lua $Version. Add it to scripts\setup-lua-runtime.ps1 before shipping this version."
}

$sourceRoot = Join-Path $buildRoot "lua-$Version"
if (Test-Path -LiteralPath $sourceRoot) {
    $resolvedSourceRoot = (Resolve-Path -LiteralPath $sourceRoot).Path
    $resolvedBuildRoot = (Resolve-Path -LiteralPath $buildRoot).Path
    if (-not $resolvedSourceRoot.StartsWith($resolvedBuildRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to clean unexpected Lua source path: $resolvedSourceRoot"
    }
    Remove-Item -LiteralPath $sourceRoot -Recurse -Force
}

tar -xzf $archivePath -C $buildRoot
if (-not (Test-Path -LiteralPath (Join-Path $sourceRoot "src") -PathType Container)) {
    throw "Extracted Lua source is missing src directory: $sourceRoot"
}

$vcvars = Find-VcVars64
if (-not $vcvars) {
    throw "vcvars64.bat not found. Install Visual Studio Build Tools with the MSVC C++ toolchain."
}

$buildBat = Join-Path $env:TEMP "killengine_build_lua_$Version.bat"
$srcDir = Join-Path $sourceRoot "src"
$outDir = $runtimeRoot

$batch = @"
@echo off
setlocal
call "$vcvars" >nul 2>&1
set "CL="
set "INCLUDE=%INCLUDE%"
cd /d "$srcDir"
del /q *.obj *.lib lua.exe luac.exe 2>nul
for %%f in (*.c) do (
  if /I not "%%f"=="lua.c" if /I not "%%f"=="luac.c" cl /nologo /O2 /MD /c "%%f"
  if errorlevel 1 exit /b 1
)
lib /nologo /OUT:lua_static.lib *.obj
if errorlevel 1 exit /b 1
cl /nologo /O2 /MD /c lua.c
if errorlevel 1 exit /b 1
link /nologo /OUT:lua.exe lua.obj lua_static.lib
if errorlevel 1 exit /b 1
if not exist "$outDir" mkdir "$outDir"
copy /Y lua.exe "$outDir\lua.exe" >nul
copy /Y ..\README "$outDir\README.txt" >nul 2>nul
copy /Y ..\doc\readme.html "$outDir\readme.html" >nul 2>nul
endlocal
"@

Set-Content -Path $buildBat -Value $batch -Encoding ASCII
try {
    & cmd /c $buildBat
    if ($LASTEXITCODE -ne 0) {
        throw "Lua build failed with exit code $LASTEXITCODE."
    }
} finally {
    Remove-Item -LiteralPath $buildBat -ErrorAction SilentlyContinue
}

$luaExe = Join-Path $runtimeRoot "lua.exe"
if (-not (Test-Path -LiteralPath $luaExe -PathType Leaf)) {
    throw "Lua build completed but lua.exe was not staged to $luaExe."
}

$versionOutput = & $luaExe -v 2>&1
Write-Host "Lua runtime ready: $luaExe" -ForegroundColor Green
Write-Host $versionOutput -ForegroundColor Cyan
