# Automated tests for the workspace history scheduling/fingerprint/diff logic
# (ui/src/stores/workspaceHistoryScheduling.ts, UX-PRODUIT-13,
# docs/PHASE_TRACKER.md): fingerprint determinism (exportedAt excluded, key
# order irrelevant), the 2s-debounce/30s-throttle automatic capture decision,
# and the by-id diff (never coalesced by name).
#
# ui/ has no JS/TS test runner configured. Rather than reimplement the logic
# a second time in the test (drift risk), this transpiles the real .ts module
# with the esbuild binary already vendored by Vite (ui/node_modules/.bin/esbuild)
# and runs the transpiled module directly with node -- same pattern as
# scripts/test-workspace-import-validation.ps1.
#
# Usage:
#   .\scripts\test-workspace-history-scheduling.ps1

$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$sourceModule = Join-Path $repoRoot "ui\src\stores\workspaceHistoryScheduling.ts"
$testScript = Join-Path $repoRoot "scripts\test-workspace-history-scheduling.mjs"
$esbuild = Join-Path $repoRoot "ui\node_modules\.bin\esbuild.cmd"

if (-not (Test-Path -LiteralPath $sourceModule -PathType Leaf)) {
    throw "Missing source module: $sourceModule"
}
if (-not (Test-Path -LiteralPath $esbuild -PathType Leaf)) {
    throw "esbuild not found at $esbuild -- run 'npm install' in ui/ first (esbuild ships with Vite, no extra dependency needed)."
}

$tempDir = Join-Path ([System.IO.Path]::GetTempPath()) ("killengine_workspace_history_scheduling_test_" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $tempDir -Force | Out-Null
$transpiled = Join-Path $tempDir "workspaceHistoryScheduling.mjs"

try {
    Write-Host "[RUN] Transpile workspaceHistoryScheduling.ts" -ForegroundColor Cyan
    & $esbuild $sourceModule --format=esm --outfile=$transpiled
    if ($LASTEXITCODE -ne 0) {
        throw "esbuild failed with code $LASTEXITCODE"
    }
    if (-not (Test-Path -LiteralPath $transpiled -PathType Leaf)) {
        throw "esbuild did not produce $transpiled"
    }
    Write-Host "[OK]  Transpile workspaceHistoryScheduling.ts" -ForegroundColor Green

    Write-Host "[RUN] Workspace history scheduling assertions" -ForegroundColor Cyan
    node $testScript $transpiled
    if ($LASTEXITCODE -ne 0) {
        throw "Workspace history scheduling assertions failed (node exit code $LASTEXITCODE)"
    }
    Write-Host "[OK]  Workspace history scheduling assertions" -ForegroundColor Green
} finally {
    Remove-Item -LiteralPath $tempDir -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host "Workspace history scheduling tests complete." -ForegroundColor Green
