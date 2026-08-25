# KillEngine line ending checker (report-only by default).
# Usage:
#   .\scripts\check-line-endings.ps1
#   .\scripts\check-line-endings.ps1 -FailOnMixed
#   .\scripts\check-line-endings.ps1 -FailOnMixed -FailOnLfOnly

param(
    [string[]]$Path = @("AGENTS.md", "ai", "apps", "core", "docs", "scripts", "tests", "ui/src"),
    [string[]]$Extensions = @(".cpp", ".h", ".hpp", ".cc", ".cxx", ".ts", ".vue", ".js", ".mjs", ".cjs", ".json", ".md", ".ps1", ".psm1", ".lua", ".cmake", ".txt"),
    [switch]$IncludeAllTracked,
    [switch]$ShowAll,
    [switch]$FailOnMixed,
    [switch]$FailOnLfOnly
)

$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path

function Get-TrackedFiles {
    $args = @("-C", $repoRoot, "ls-files", "--")
    if (-not $IncludeAllTracked) {
        $args += $Path
    }

    $files = & git @args
    if ($LASTEXITCODE -ne 0) {
        throw "git ls-files failed with exit code $LASTEXITCODE"
    }

    foreach ($file in $files) {
        if ([string]::IsNullOrWhiteSpace($file)) {
            continue
        }

        $extension = [System.IO.Path]::GetExtension($file)
        if ($Extensions -contains $extension) {
            $file
        }
    }
}

function Measure-LineEndings {
    param([Parameter(Mandatory = $true)][string]$RelativePath)

    $fullPath = Join-Path $repoRoot $RelativePath
    $bytes = [System.IO.File]::ReadAllBytes($fullPath)

    $crlf = 0
    $lfOnly = 0
    $crOnly = 0

    for ($i = 0; $i -lt $bytes.Length; $i++) {
        if ($bytes[$i] -eq 13) {
            if (($i + 1) -lt $bytes.Length -and $bytes[$i + 1] -eq 10) {
                $crlf++
                $i++
            } else {
                $crOnly++
            }
        } elseif ($bytes[$i] -eq 10) {
            $lfOnly++
        }
    }

    $style = if ($crlf -eq 0 -and $lfOnly -eq 0 -and $crOnly -eq 0) {
        "none"
    } elseif ($crlf -gt 0 -and $lfOnly -eq 0 -and $crOnly -eq 0) {
        "CRLF"
    } elseif ($crlf -eq 0 -and $lfOnly -gt 0 -and $crOnly -eq 0) {
        "LF"
    } elseif ($crlf -eq 0 -and $lfOnly -eq 0 -and $crOnly -gt 0) {
        "CR"
    } else {
        "mixed"
    }

    [pscustomobject]@{
        Path = $RelativePath
        Style = $style
        CRLF = $crlf
        LFOnly = $lfOnly
        CROnly = $crOnly
    }
}

$results = @(Get-TrackedFiles | Sort-Object | ForEach-Object { Measure-LineEndings -RelativePath $_ })
$interesting = @($results | Where-Object { $_.Style -ne "CRLF" -and $_.Style -ne "none" })
$mixed = @($results | Where-Object { $_.Style -eq "mixed" -or $_.Style -eq "CR" })
$lfOnly = @($results | Where-Object { $_.Style -eq "LF" })

Write-Host "Line ending report:" -ForegroundColor Cyan
Write-Host "  Files checked: $($results.Count)"
Write-Host "  CRLF:         $(@($results | Where-Object { $_.Style -eq "CRLF" }).Count)"
Write-Host "  LF only:      $($lfOnly.Count)"
Write-Host "  Mixed/CR:     $($mixed.Count)"
Write-Host "  No newline:   $(@($results | Where-Object { $_.Style -eq "none" }).Count)"

$toShow = if ($ShowAll) { $results } else { $interesting }
if ($toShow.Count -gt 0) {
    Write-Host ""
    Write-Host "Details:" -ForegroundColor Cyan
    $toShow |
        Sort-Object Style, Path |
        Format-Table -AutoSize Path, Style, CRLF, LFOnly, CROnly
} else {
    Write-Host ""
    Write-Host "All checked files are CRLF or contain no line endings." -ForegroundColor Green
}

$errors = New-Object System.Collections.Generic.List[string]
if ($FailOnMixed -and $mixed.Count -gt 0) {
    $errors.Add("Mixed/CR line endings found in $($mixed.Count) file(s).") | Out-Null
}
if ($FailOnLfOnly -and $lfOnly.Count -gt 0) {
    $errors.Add("LF-only line endings found in $($lfOnly.Count) file(s).") | Out-Null
}

if ($errors.Count -gt 0) {
    foreach ($errorMessage in $errors) {
        Write-Host "ERROR: $errorMessage" -ForegroundColor Red
    }
    throw "Line ending check failed."
}
