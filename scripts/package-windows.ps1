# KillEngine Windows packaging script
# Usage:
#   .\scripts\package-windows.ps1
#   .\scripts\package-windows.ps1 -SkipBuild
#   .\scripts\package-windows.ps1 -IncludeModel

param(
    [switch]$SkipBuild,
    [switch]$IncludeModel
)

$ErrorActionPreference = "Stop"

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$buildBin = Join-Path $repoRoot "build\bin"
$distRoot = Join-Path $repoRoot "dist"
$packageName = "KillEngine-portable"
$packageRoot = Join-Path $distRoot $packageName
$zipPath = Join-Path $distRoot "$packageName.zip"

function Copy-ItemIfExists {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Destination
    )

    if (Test-Path $Path) {
        Copy-Item -LiteralPath $Path -Destination $Destination -Recurse -Force
    }
}

if (-not $SkipBuild) {
    & (Join-Path $repoRoot "scripts\build.ps1")
}

$exePath = Join-Path $buildBin "KillEngine.exe"
if (-not (Test-Path $exePath)) {
    throw "KillEngine.exe was not found at $exePath. Run .\scripts\build.ps1 first."
}

if (Test-Path $packageRoot) {
    $resolvedPackageRoot = (Resolve-Path $packageRoot).Path
    $resolvedDistRoot = if (Test-Path $distRoot) { (Resolve-Path $distRoot).Path } else { $distRoot }
    if (-not $resolvedPackageRoot.StartsWith($resolvedDistRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to clean unexpected package path: $resolvedPackageRoot"
    }
    Remove-Item -LiteralPath $packageRoot -Recurse -Force
}

New-Item -ItemType Directory -Force -Path $packageRoot | Out-Null

$excludedNames = @(
    "killengine_unit_tests.exe",
    "killengine_unit_tests.pdb",
    "KillEngineTestTarget.exe",
    "KillEngineTestTarget.pdb",
    "lz4.pdb"
)

$excludedExtensions = @(".pdb", ".ilk", ".exp", ".lib")

Get-ChildItem -LiteralPath $buildBin -Force | ForEach-Object {
    if ($excludedNames -contains $_.Name) {
        return
    }
    if (-not $_.PSIsContainer -and $excludedExtensions -contains $_.Extension) {
        return
    }
    if (-not $_.PSIsContainer -and $_.Name -match "d\.dll$") {
        return
    }

    Copy-Item -LiteralPath $_.FullName -Destination $packageRoot -Recurse -Force
}

$debugPatterns = @(
    "*.pdb",
    "*.ilk",
    "*.exp",
    "*.lib",
    "*.debug.*",
    "*d.dll",
    "*d.exe",
    "msvcp*d*.dll",
    "vcruntime*d*.dll",
    "ucrt*d*.dll",
    "concrt*d*.dll"
)

foreach ($pattern in $debugPatterns) {
    Get-ChildItem -LiteralPath $packageRoot -Recurse -File -Filter $pattern -ErrorAction SilentlyContinue |
        Remove-Item -Force
}

Copy-ItemIfExists -Path (Join-Path $repoRoot "README.md") -Destination $packageRoot
Copy-ItemIfExists -Path (Join-Path $repoRoot "LICENSE") -Destination $packageRoot
Copy-ItemIfExists -Path (Join-Path $repoRoot "KILLENGINE_PROJECT_SPEC.md") -Destination $packageRoot
Copy-ItemIfExists -Path (Join-Path $repoRoot "docs\PHASE_TRACKER.md") -Destination $packageRoot
Copy-ItemIfExists -Path (Join-Path $repoRoot "docs\USER_GUIDE.md") -Destination $packageRoot

$modelsOut = Join-Path $packageRoot "models"
New-Item -ItemType Directory -Force -Path $modelsOut | Out-Null
Copy-ItemIfExists -Path (Join-Path $repoRoot "models\README.md") -Destination $modelsOut

if ($IncludeModel) {
    Get-ChildItem -LiteralPath (Join-Path $repoRoot "models") -File -Include "*.gguf" -ErrorAction SilentlyContinue |
        ForEach-Object {
            Copy-Item -LiteralPath $_.FullName -Destination $modelsOut -Force
        }
}

$packageReadme = @"
KillEngine portable package
===========================

Run:
  KillEngine.exe

Notes:
  - This package is intended for local/offline testing.
  - GGUF models are not included unless package-windows.ps1 is run with -IncludeModel.
  - Place qwen.gguf in the models folder or set KILLENGINE_QWEN_GGUF.
  - Logs and profiles are stored under the Windows local app data folder.
  - Read USER_GUIDE.md for the V1 user workflow.
"@

Set-Content -Path (Join-Path $packageRoot "PACKAGE_README.txt") -Value $packageReadme -Encoding ASCII

if (Test-Path $zipPath) {
    Remove-Item -LiteralPath $zipPath -Force
}

Compress-Archive -Path (Join-Path $packageRoot "*") -DestinationPath $zipPath -Force

Write-Host "Portable package ready:" -ForegroundColor Green
Write-Host "  Folder: $packageRoot" -ForegroundColor Cyan
Write-Host "  Zip:    $zipPath" -ForegroundColor Cyan
