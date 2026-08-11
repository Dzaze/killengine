# KillEngine V1 release validation
# Usage:
#   .\scripts\release-check.ps1
#   .\scripts\release-check.ps1 -SkipUi -Package

param(
    [switch]$SkipUi,
    [switch]$SkipConfigure,
    [switch]$SkipTests,
    [switch]$Package,
    [switch]$IncludeModel
)

$ErrorActionPreference = "Stop"

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$startedAt = Get-Date

function Invoke-Step {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][scriptblock]$Script
    )

    Write-Host ""
    Write-Host "==> $Name" -ForegroundColor Cyan
    $global:LASTEXITCODE = 0
    & $Script
    if ($LASTEXITCODE -ne $null -and $LASTEXITCODE -ne 0) {
        throw "$Name failed with exit code $LASTEXITCODE"
    }
}

Push-Location $repoRoot
try {
    if (-not $SkipUi) {
        Invoke-Step "UI type-check" {
            Push-Location (Join-Path $repoRoot "ui")
            try {
                npm run type-check
            } finally {
                Pop-Location
            }
        }

        Invoke-Step "UI build" {
            Push-Location (Join-Path $repoRoot "ui")
            try {
                npm run build
            } finally {
                Pop-Location
            }
        }
    }

    if (-not $SkipConfigure) {
        Invoke-Step "CMake configure" {
            & (Join-Path $repoRoot "scripts\configure.ps1")
        }
    }

    Invoke-Step "C++ build" {
        & (Join-Path $repoRoot "scripts\build.ps1")
    }

    if (-not $SkipTests) {
        $unitTests = Join-Path $repoRoot "build\bin\killengine_unit_tests.exe"
        $integrationTests = Join-Path $repoRoot "build\bin\killengine_integration_tests.exe"

        Invoke-Step "Unit tests" {
            & $unitTests
        }

        Invoke-Step "Integration tests" {
            & $integrationTests
        }
    }

    if ($Package) {
        Invoke-Step "Portable package" {
            if ($IncludeModel) {
                & (Join-Path $repoRoot "scripts\package-windows.ps1") -SkipBuild -IncludeModel
            } else {
                & (Join-Path $repoRoot "scripts\package-windows.ps1") -SkipBuild
            }
        }
    }

    $duration = New-TimeSpan -Start $startedAt -End (Get-Date)
    Write-Host ""
    Write-Host ("Release check passed in {0:mm\:ss}." -f $duration) -ForegroundColor Green
} finally {
    Pop-Location
}
