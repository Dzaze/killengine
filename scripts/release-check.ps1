# KillEngine V1 release validation
# Usage:
#   .\scripts\release-check.ps1
#   .\scripts\release-check.ps1 -SkipUi -Package
#   .\scripts\release-check.ps1 -Package -ExcludeModel
#   .\scripts\release-check.ps1 -Package -RequireSigning   (official release: fail if KillEngine.exe ships unsigned)

param(
    [switch]$SkipUi,
    [switch]$SkipConfigure,
    [switch]$SkipTests,
    [switch]$SkipLaunchSmoke,
    [switch]$Package,
    [switch]$ExcludeModel,
    [switch]$RequireSigning
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
