# KillEngine embedded AI layout verifier
# Usage:
#   .\scripts\verify-ai-layout.ps1
#   .\scripts\verify-ai-layout.ps1 -LayoutRoot .\dist\KillEngine-portable

param(
    [string]$LayoutRoot,
    [string[]]$RequiredAgents = @("assistant", "auto_resolver"),
    [switch]$AllowMissingRuntime
)

$ErrorActionPreference = "Stop"

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
if (-not $LayoutRoot) {
    $LayoutRoot = Join-Path $repoRoot "build\bin"
}

$resolvedRoot = if (Test-Path -LiteralPath $LayoutRoot) {
    (Resolve-Path -LiteralPath $LayoutRoot).Path
} else {
    $LayoutRoot
}

$errors = New-Object System.Collections.Generic.List[string]
$warnings = New-Object System.Collections.Generic.List[string]

function Add-ErrorMessage {
    param([string]$Message)
    $script:errors.Add($Message) | Out-Null
}

function Add-WarningMessage {
    param([string]$Message)
    $script:warnings.Add($Message) | Out-Null
}

function Resolve-ManifestPath {
    param(
        [Parameter(Mandatory = $true)][string]$BaseDir,
        [Parameter(Mandatory = $true)][string]$RelativePath
    )

    if ([string]::IsNullOrWhiteSpace($RelativePath)) {
        return $null
    }

    $combined = Join-Path $BaseDir $RelativePath
    return [System.IO.Path]::GetFullPath($combined)
}

Write-Host "Verifying KillEngine embedded AI layout:" -ForegroundColor Cyan
Write-Host "  Root: $resolvedRoot"

if (-not (Test-Path -LiteralPath $resolvedRoot -PathType Container)) {
    Add-ErrorMessage "Layout root does not exist: $resolvedRoot"
} else {
    $llamaCli = Join-Path $resolvedRoot "llama-cli.exe"
    if (-not $AllowMissingRuntime -and -not (Test-Path -LiteralPath $llamaCli -PathType Leaf)) {
        Add-ErrorMessage "Missing embedded runtime: llama-cli.exe"
    }

    $modelRoot = Join-Path $resolvedRoot "model"
    if (-not (Test-Path -LiteralPath $modelRoot -PathType Container)) {
        Add-ErrorMessage "Missing model folder: model\"
    } else {
        $partials = @(Get-ChildItem -LiteralPath $modelRoot -Recurse -File -Filter "*.partial" -ErrorAction SilentlyContinue)
        if ($partials.Count -gt 0) {
            Add-ErrorMessage "Partial model downloads must not be packaged: $($partials.FullName -join ', ')"
        }

        $ggufFiles = @(Get-ChildItem -LiteralPath $modelRoot -Recurse -File -Filter "*.gguf" -ErrorAction SilentlyContinue)
        if ($ggufFiles.Count -eq 0) {
            Add-ErrorMessage "No GGUF model found under model\"
        }

        foreach ($agentId in $RequiredAgents) {
            $agentDir = Join-Path $modelRoot $agentId
            $manifestPath = Join-Path $agentDir "MODEL_MANIFEST.json"
            if (-not (Test-Path -LiteralPath $agentDir -PathType Container)) {
                Add-ErrorMessage "Missing required AI agent folder: model\$agentId"
                continue
            }
            if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
                Add-ErrorMessage "Missing required AI agent manifest: model\$agentId\MODEL_MANIFEST.json"
                continue
            }

            try {
                $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
            } catch {
                Add-ErrorMessage "Invalid JSON manifest for agent '$agentId': $($_.Exception.Message)"
                continue
            }

            if ([string]::IsNullOrWhiteSpace($manifest.id)) {
                Add-ErrorMessage "Manifest for agent '$agentId' has no id."
            } elseif ($manifest.id -ne $agentId) {
                Add-WarningMessage "Manifest id '$($manifest.id)' does not match folder '$agentId'."
            }

            if ([string]::IsNullOrWhiteSpace($manifest.role)) {
                Add-ErrorMessage "Manifest for agent '$agentId' has no role."
            }
            if ([string]::IsNullOrWhiteSpace($manifest.provider)) {
                Add-ErrorMessage "Manifest for agent '$agentId' has no provider."
            }

            $modelPath = Resolve-ManifestPath -BaseDir $agentDir -RelativePath $manifest.modelPath
            if (-not $modelPath) {
                Add-ErrorMessage "Manifest for agent '$agentId' has no modelPath."
            } elseif (-not (Test-Path -LiteralPath $modelPath -PathType Leaf)) {
                Add-ErrorMessage "Manifest for agent '$agentId' points to a missing model: $modelPath"
            } elseif ([System.IO.Path]::GetExtension($modelPath) -ine ".gguf") {
                Add-ErrorMessage "Manifest for agent '$agentId' must point to a .gguf model: $modelPath"
            }

            if ($manifest.runtimePath) {
                $runtimePath = Resolve-ManifestPath -BaseDir $agentDir -RelativePath $manifest.runtimePath
                if (-not (Test-Path -LiteralPath $runtimePath -PathType Leaf)) {
                    Add-ErrorMessage "Manifest for agent '$agentId' points to a missing runtime: $runtimePath"
                }
            }
        }
    }
}

foreach ($warning in $warnings) {
    Write-Host "WARNING: $warning" -ForegroundColor Yellow
}

if ($errors.Count -gt 0) {
    foreach ($errorMessage in $errors) {
        Write-Host "ERROR: $errorMessage" -ForegroundColor Red
    }
    throw "Embedded AI layout validation failed with $($errors.Count) error(s)."
}

Write-Host "Embedded AI layout OK." -ForegroundColor Green
