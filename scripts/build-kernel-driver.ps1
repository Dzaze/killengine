param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$solution = Join-Path $repoRoot 'tools\kernel_driver\KillEngineKernel\KillEngineKernel.sln'

$msbuildCandidates = @(
    "${env:ProgramFiles(x86)}\Microsoft Visual Studio\18\BuildTools\MSBuild\Current\Bin\amd64\MSBuild.exe",
    "${env:ProgramFiles(x86)}\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe",
    "${env:ProgramFiles}\Microsoft Visual Studio\18\BuildTools\MSBuild\Current\Bin\amd64\MSBuild.exe",
    "${env:ProgramFiles}\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe",
    "${env:ProgramFiles}\Microsoft Visual Studio\2026\BuildTools\MSBuild\Current\Bin\amd64\MSBuild.exe",
    "${env:ProgramFiles}\Microsoft Visual Studio\2026\Community\MSBuild\Current\Bin\amd64\MSBuild.exe",
    "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2026\BuildTools\MSBuild\Current\Bin\amd64\MSBuild.exe",
    "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2026\Community\MSBuild\Current\Bin\amd64\MSBuild.exe",
    "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\amd64\MSBuild.exe",
    "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\amd64\MSBuild.exe",
    "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2019\BuildTools\MSBuild\Current\Bin\amd64\MSBuild.exe",
    "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2019\Community\MSBuild\Current\Bin\amd64\MSBuild.exe"
)
$msbuild = $msbuildCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $msbuild) {
    throw "MSBuild introuvable. Installe Visual Studio Build Tools avec le workload WDK."
}

$wdkKmHeaders = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\Include" -Directory -ErrorAction SilentlyContinue |
    Where-Object { Test-Path (Join-Path $_.FullName 'km\ntddk.h') } |
    Sort-Object Name -Descending |
    Select-Object -First 1
if (-not $wdkKmHeaders) {
    throw "WDK kernel headers introuvables (ntddk.h). Installe le Windows Driver Kit pour construire KillEngineKernel.sys."
}

Write-Host "Building KillEngineKernel ($Configuration|x64)..."
& $msbuild $solution /m /restore /p:Configuration=$Configuration /p:Platform=x64
if ($LASTEXITCODE -ne 0) {
    throw "Build du driver échoué avec le code $LASTEXITCODE."
}

$sys = Join-Path $repoRoot "tools\kernel_driver\build\kernel\$Configuration\KillEngineKernel.sys"
if (-not (Test-Path $sys)) {
    $sys = Get-ChildItem (Join-Path $repoRoot 'tools\kernel_driver\build') -Recurse -Filter KillEngineKernel.sys -ErrorAction SilentlyContinue |
        Select-Object -First 1 -ExpandProperty FullName
}
if (-not $sys) {
    throw "Build terminé mais KillEngineKernel.sys est introuvable."
}

Write-Host "Driver built: $sys"
