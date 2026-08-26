# KillEngine recent targeted regression tests
# Usage:
#   .\scripts\test-recent-targeted.ps1
#   .\scripts\test-recent-targeted.ps1 -SkipIntegration

param(
    [switch]$SkipIntegration
)

$ErrorActionPreference = "Stop"

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$unitTests = Join-Path $repoRoot "build\bin\killengine_unit_tests.exe"
$integrationTests = Join-Path $repoRoot "build\bin\killengine_integration_tests.exe"

function Invoke-TestBinary {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$ExePath,
        [Parameter(Mandatory = $true)][string]$Filter
    )

    if (-not (Test-Path -LiteralPath $ExePath -PathType Leaf)) {
        throw "$Name not found at $ExePath. Run .\scripts\build.ps1 first."
    }

    Write-Host ""
    Write-Host "==> $Name" -ForegroundColor Cyan
    & $ExePath --gtest_filter=$Filter
    if ($LASTEXITCODE -ne 0) {
        throw "$Name failed with exit code $LASTEXITCODE"
    }
}

Push-Location $repoRoot
try {
    Invoke-TestBinary `
        -Name "Recent targeted unit tests" `
        -ExePath $unitTests `
        -Filter "AIToolRegistryTest.*:AIEngineContextualFallbackTest.TrainerFastPath*:DisplaySourceClassifier.*"

    if (-not $SkipIntegration) {
        Invoke-TestBinary `
            -Name "Recent targeted integration tests" `
            -ExePath $integrationTests `
            -Filter "DisplayVsSourceTargetTest.*"
    }
} finally {
    Pop-Location
}

Write-Host ""
Write-Host "Recent targeted tests passed." -ForegroundColor Green
