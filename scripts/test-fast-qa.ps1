# KillEngine fast QA gate for parallel-agent sessions.
# Runs the recent targeted regression suite plus the safe automation pipe battery.
# Usage:
#   .\scripts\test-fast-qa.ps1
#   .\scripts\test-fast-qa.ps1 -SkipAutomationPipe
#   .\scripts\test-fast-qa.ps1 -SkipRecentTargeted
#   .\scripts\test-fast-qa.ps1 -SkipToolRegistry

param(
    [switch]$SkipRecentTargeted,
    [switch]$SkipAutomationPipe,
    [switch]$SkipToolRegistry
)

$ErrorActionPreference = "Stop"

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$startedAt = Get-Date

if ($SkipRecentTargeted -and $SkipAutomationPipe) {
    throw "Nothing to run: both -SkipRecentTargeted and -SkipAutomationPipe were provided."
}

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
    if (-not $SkipRecentTargeted) {
        Invoke-Step "Recent targeted tests" {
            & (Join-Path $repoRoot "scripts\test-recent-targeted.ps1") -SkipToolRegistry:$SkipToolRegistry
        }
    }

    if (-not $SkipAutomationPipe) {
        Invoke-Step "Automation pipe safe-methods battery" {
            & (Join-Path $repoRoot "scripts\test-automation-pipe-safe-methods.ps1")
        }
    }

    $duration = New-TimeSpan -Start $startedAt -End (Get-Date)
    Write-Host ""
    Write-Host ("Fast QA gate passed in {0:mm\:ss}." -f $duration) -ForegroundColor Green
} finally {
    Pop-Location
}
