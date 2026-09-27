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
    Write-Host "Recent targeted tests and the automation pipe battery are both skipped -- only the static backend contract check (TS-side) will run." -ForegroundColor Yellow
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

    # UX-PRODUIT-14A (docs/PHASE_TRACKER.md) : contrôle statique TS toujours
    # exécuté ; -SkipAutomationPipe fait aussi sauter l'inventaire runtime C++
    # dépendant du pipe (le script le signale lui-même en SKIPPED, pas une
    # disparition silencieuse de l'étape).
    Invoke-Step "Backend contract (C++/TS/mock)" {
        & (Join-Path $repoRoot "scripts\test-backend-contract.ps1") -SkipAutomationPipe:$SkipAutomationPipe
    }

    # UX-PRODUIT-14B (docs/PHASE_TRACKER.md) : manifeste déclaratif des
    # comportements réels (pas seulement leurs signatures, déjà couvertes par
    # 14A ci-dessus). Le checker et la garde requestId simulée sont purs/
    # déterministes -- toujours exécutés. Le manifeste live a besoin d'une
    # instance KillEngine.exe/KillEngineTestTarget.exe dédiée, donc respecte
    # -SkipAutomationPipe comme l'étape 14A juste au-dessus (même convention).
    Invoke-Step "Behavior manifest checker (pure)" {
        & (Join-Path $repoRoot "scripts\test-behavior-manifest-check.ps1")
    }
    Invoke-Step "Scan race-guard assertions (simulated, deterministic)" {
        & (Join-Path $repoRoot "scripts\test-scan-race-guards.ps1")
    }
    if (-not $SkipAutomationPipe) {
        Invoke-Step "Behavior manifest (live)" {
            & (Join-Path $repoRoot "scripts\test-behavior-manifest.ps1")
        }
    }

    $duration = New-TimeSpan -Start $startedAt -End (Get-Date)
    Write-Host ""
    Write-Host ("Fast QA gate passed in {0:mm\:ss}." -f $duration) -ForegroundColor Green
} finally {
    Pop-Location
}
