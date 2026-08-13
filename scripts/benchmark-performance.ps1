# KillEngine — Benchmark de performance orchestré
# Usage:
#   .\scripts\benchmark-performance.ps1
#   .\scripts\benchmark-performance.ps1 -LargeCandidates 1000000
#   .\scripts\benchmark-performance.ps1 -SkipBuild -JsonOutput
#
# Lance KillEngineBenchmark.exe (voir tests/benchmarks/benchmark_main.cpp)
# qui mesure scan exact, scan multi-type, unknown snapshot et gros volumes
# de candidats sur KillEngineTestTarget.exe.
#
# Voir docs/PERFORMANCE_BENCHMARKS.md pour l'interprétation des résultats.

param(
    [switch]$SkipBuild,
    [switch]$SkipConfigure,
    [switch]$JsonOutput,
    [int64]$LargeCandidates = 500000,
    [string]$OutputDir
)

$ErrorActionPreference = "Stop"

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$buildDir = Join-Path $repoRoot "build"
$binDir = Join-Path $buildDir "bin"
$benchmarkExe = Join-Path $binDir "KillEngineBenchmark.exe"

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

# --- Résolution du répertoire de sortie ---
if (-not $OutputDir) {
    $OutputDir = Join-Path $repoRoot "docs\benchmark-results"
}
if (-not (Test-Path $OutputDir)) {
    New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
}

Push-Location $repoRoot
try {
    # --- Build ---
    if (-not $SkipBuild) {
        if (-not $SkipConfigure) {
            $cacheFile = Join-Path $buildDir "CMakeCache.txt"
            if (-not (Test-Path $cacheFile)) {
                Invoke-Step "CMake configure" {
                    & (Join-Path $repoRoot "scripts\configure.ps1")
                }
            } else {
                Write-Host "CMake cache found, skipping configure." -ForegroundColor DarkGray
            }
        }

        Invoke-Step "Build KillEngineBenchmark" {
            & (Join-Path $repoRoot "scripts\build.ps1")
        }
    }

    if (-not (Test-Path $benchmarkExe)) {
        throw "KillEngineBenchmark.exe not found at: $benchmarkExe`nBuild it first with -SkipBuild omitted."
    }

    # --- Lancement du benchmark ---
    $startedAt = Get-Date
    $args = @()
    if ($LargeCandidates -gt 0) {
        $args += @("--large-candidates", $LargeCandidates)
    }
    if ($JsonOutput) {
        $args += "--json"
    }

    Write-Host ""
    Write-Host "==> Running benchmark: $benchmarkExe $($args -join ' ')" -ForegroundColor Cyan

    $rawOutput = & $benchmarkExe @args 2>&1
    $exitCode = $LASTEXITCODE
    $duration = New-TimeSpan -Start $startedAt -End (Get-Date)

    # Affichage brut
    $rawOutput | ForEach-Object { Write-Host $_ }

    if ($exitCode -ne 0) {
        Write-Host ""
        Write-Host "Benchmark exited with code $exitCode after $([math]::Round($duration.TotalSeconds, 2))s." -ForegroundColor Red
        throw "Benchmark failed."
    }

    # --- Sauvegarde des résultats ---
    $timestamp = (Get-Date).ToString("yyyyMMdd_HHmmss")
    $hostName = $env:COMPUTERNAME

    # Sortie texte brute
    $txtPath = Join-Path $OutputDir "benchmark_${hostName}_${timestamp}.txt"
    $rawOutput | Out-File -FilePath $txtPath -Encoding UTF8

    # Extraction métriques structurées (depuis les lignes METRIC)
    $metricsPath = Join-Path $OutputDir "benchmark_${hostName}_${timestamp}.csv"
    "scenario,duration_ms,candidates,bytes,candidates_per_s,bytes_per_s,note" | Out-File -FilePath $metricsPath -Encoding UTF8

    foreach ($line in $rawOutput) {
        if ($line -match '^METRIC\t(.+)$') {
            $rest = $matches[1]
            # Format: name\tkey=val\tkey=val...
            $parts = $rest -split "`t"
            if ($parts.Count -lt 1) { continue }
            $scenario = $parts[0]
            $durationMs = ""; $candidates = ""; $bytes = ""; $cps = ""; $bps = ""; $note = ""
            foreach ($p in $parts[1..($parts.Count - 1)]) {
                if ($p -match '^duration_ms=(.+)$') { $durationMs = $matches[1] }
                elseif ($p -match '^candidates=(.+)$') { $candidates = $matches[1] }
                elseif ($p -match '^bytes=(.+)$') { $bytes = $matches[1] }
                elseif ($p -match '^candidates_per_s=(.+)$') { $cps = $matches[1] }
                elseif ($p -match '^bytes_per_s=(.+)$') { $bps = $matches[1] }
                elseif ($p -match '^note=(.*)$') { $note = $matches[1] }
            }
            "$scenario,$durationMs,$candidates,$bytes,$cps,$bps,`"$note`"" | Out-File -FilePath $metricsPath -Encoding UTF8 -Append
        }
    }

    Write-Host ""
    Write-Host "==> Benchmark complete in $([math]::Round($duration.TotalSeconds, 2))s." -ForegroundColor Green
    Write-Host "    Raw output : $txtPath" -ForegroundColor Cyan
    Write-Host "    CSV metrics: $metricsPath" -ForegroundColor Cyan

} finally {
    Pop-Location
}