# KillEngine V1 release validation
# Usage:
#   .\scripts\release-check.ps1
#   .\scripts\release-check.ps1 -SkipUi -Package
#   .\scripts\release-check.ps1 -Package -ExcludeModel
#   .\scripts\release-check.ps1 -Package -RequireSigning   (official release: fail if KillEngine.exe ships unsigned)
#   .\scripts\release-check.ps1 -IncludeLuaExamples        (optional, best-effort: never fails the gate if runtime/lua/ is absent)
#
# -IncludeLuaExamples runs scripts\test-lua-examples.ps1 WITHOUT -RequirePipe (pipe-backed
# strictness is a separate, explicit command -- run it directly when you want that guarantee):
#   .\scripts\test-lua-examples.ps1 -RequirePipe   (needs KillEngine.exe already running with KILLENGINE_AUTOMATION_PIPE=1)

param(
    [switch]$SkipUi,
    [switch]$SkipConfigure,
    [switch]$SkipTests,
    [switch]$SkipLaunchSmoke,
    [switch]$Package,
    [switch]$ExcludeModel,
    [switch]$RequireSigning,
    [switch]$IncludeLuaExamples
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

function Invoke-OptionalStep {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][scriptblock]$Script
    )

    Write-Host ""
    Write-Host "==> $Name (optional, non-blocking)" -ForegroundColor Cyan
    try {
        & $Script
        Write-Host "$Name : OK" -ForegroundColor Green
    } catch {
        Write-Warning "$Name skipped/failed, not failing the release check: $($_.Exception.Message)"
    }
}

function Invoke-KillEngineLaunchSmoke {
    param(
        [Parameter(Mandatory = $true)][string]$ExePath
    )

    if (-not (Test-Path $ExePath)) {
        throw "KillEngine.exe not found at $ExePath"
    }

    $process = Start-Process -FilePath $ExePath -PassThru
    try {
        Start-Sleep -Seconds 5
        $alive = Get-Process -Id $process.Id -ErrorAction SilentlyContinue
        if (-not $alive) {
            throw "KillEngine.exe exited during launch smoke."
        }
    } finally {
        $alive = Get-Process -Id $process.Id -ErrorAction SilentlyContinue
        if ($alive) {
            $cim = Get-CimInstance Win32_Process -Filter "ProcessId=$($process.Id)"
            if ($cim) {
                Invoke-CimMethod -InputObject $cim -MethodName Terminate | Out-Null
            } else {
                Stop-Process -Id $process.Id -Force
            }
        }
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

    Invoke-Step "Embedded AI layout" {
        & (Join-Path $repoRoot "scripts\verify-ai-layout.ps1") -LayoutRoot (Join-Path $repoRoot "build\bin")
    }

    if (-not $SkipLaunchSmoke) {
        Invoke-Step "KillEngine launch smoke" {
            $exe = Join-Path $repoRoot "build\bin\KillEngine.exe"
            Invoke-KillEngineLaunchSmoke -ExePath $exe
        }
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

    if ($IncludeLuaExamples) {
        # Best-effort by design: a missing Lua runtime (runtime/lua/ not
        # provisioned on this machine) must never fail the release check --
        # this step only exists to catch real regressions in the bundled
        # examples when the runtime happens to be there. Deliberately runs
        # WITHOUT -RequirePipe: pipe-backed strictness is a separate,
        # explicitly documented command (see docs/V1_REGRESSION_CHECKLIST.md),
        # not something release-check.ps1 enables implicitly.
        Invoke-OptionalStep "Lua examples validation" {
            & (Join-Path $repoRoot "scripts\test-lua-examples.ps1")
            if ($LASTEXITCODE -ne 0) {
                throw "test-lua-examples.ps1 exited with code $LASTEXITCODE"
            }
        }
    }

    if ($Package) {
        Invoke-Step "Portable package" {
            if ($ExcludeModel) {
                & (Join-Path $repoRoot "scripts\package-windows.ps1") -SkipBuild -ExcludeModel -RequireSigning:$RequireSigning
            } else {
                & (Join-Path $repoRoot "scripts\package-windows.ps1") -SkipBuild -RequireSigning:$RequireSigning
            }
        }

        if (-not $SkipLaunchSmoke) {
            Invoke-Step "Portable launch smoke" {
                $exe = Join-Path $repoRoot "dist\KillEngine-portable\KillEngine.exe"
                Invoke-KillEngineLaunchSmoke -ExePath $exe
            }
        }

        if (-not $ExcludeModel) {
            Invoke-Step "Portable embedded AI layout" {
                & (Join-Path $repoRoot "scripts\verify-ai-layout.ps1") -LayoutRoot (Join-Path $repoRoot "dist\KillEngine-portable")
            }
        }
    }

    $duration = New-TimeSpan -Start $startedAt -End (Get-Date)
    Write-Host ""
    Write-Host ("Release check passed in {0:mm\:ss}." -f $duration) -ForegroundColor Green
} finally {
    Pop-Location
}
