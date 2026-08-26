# KillEngine recent targeted regression tests
# Usage:
#   .\scripts\test-recent-targeted.ps1
#   .\scripts\test-recent-targeted.ps1 -List
#   .\scripts\test-recent-targeted.ps1 -SkipIntegration
#   .\scripts\test-recent-targeted.ps1 -SkipToolRegistry

param(
    [switch]$List,
    [switch]$SkipIntegration,
    [switch]$SkipToolRegistry
)

$ErrorActionPreference = "Stop"

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$unitTests = Join-Path $repoRoot "build\bin\killengine_unit_tests.exe"
$integrationTests = Join-Path $repoRoot "build\bin\killengine_integration_tests.exe"
$recentUnitSuites = @(
    "AIToolRegistryTest.*",
    "AIEngineContextualFallbackTest.TrainerFastPath*",
    "AIEngineContextualFallbackTest.FieldStabilityFastPath*",
    "DisplaySourceClassifier.*",
    "AutoAssembler.*",
    "ProfileStore.*"
)
if ($SkipToolRegistry) {
    $recentUnitSuites = $recentUnitSuites | Where-Object { $_ -ne "AIToolRegistryTest.*" }
}
$recentUnitFilter = $recentUnitSuites -join ":"
$recentIntegrationFilter = "DisplayVsSourceTargetTest.*"

function Write-RecentFilters {
    Write-Host "Recent targeted unit filter:" -ForegroundColor Cyan
    Write-Host "  $recentUnitFilter"
    if ($SkipToolRegistry) {
        Write-Host "  (AIToolRegistryTest.* skipped by request)" -ForegroundColor Yellow
    }
    if (-not $SkipIntegration) {
        Write-Host "Recent targeted integration filter:" -ForegroundColor Cyan
        Write-Host "  $recentIntegrationFilter"
    }
}

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
    Write-RecentFilters
    if ($List) {
        return
    }

    Invoke-TestBinary `
        -Name "Recent targeted unit tests" `
        -ExePath $unitTests `
        -Filter $recentUnitFilter

    if (-not $SkipIntegration) {
        Invoke-TestBinary `
            -Name "Recent targeted integration tests" `
            -ExePath $integrationTests `
            -Filter $recentIntegrationFilter
    }
} finally {
    Pop-Location
}

Write-Host ""
Write-Host "Recent targeted tests passed." -ForegroundColor Green
