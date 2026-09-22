# Automated tests for the workspace import validation logic
# (ui/src/stores/workspaceImportValidation.ts, UX-PIPE-7,
# docs/PHASE_TRACKER.md, 18/09/2026): full pre-mutation validation of a
# workspace export JSON, required to reject the diagnosed cases (malformed
# JSON, unsupported version, a structurally incomplete "investigation.active"
# that used to crash importWorkspaceJson after already corrupting store
# state) without ever exposing partially-validated data on rejection.
#
# ui/ has no JS/TS test runner configured. Rather than reimplement the
# validation a second time in the test (drift risk) or add a new test
# framework dependency, this transpiles the real .ts module with the esbuild
# binary already vendored by Vite (ui/node_modules/.bin/esbuild) and runs the
# transpiled module directly with node -- same pattern as
# scripts/test-trainer-dependencies.ps1 for trainerDependencies.ts.
#
# Usage:
#   .\scripts\test-workspace-import-validation.ps1

$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$sourceModule = Join-Path $repoRoot "ui\src\stores\workspaceImportValidation.ts"
$testScript = Join-Path $repoRoot "scripts\test-workspace-import-validation.mjs"
$esbuild = Join-Path $repoRoot "ui\node_modules\.bin\esbuild.cmd"

if (-not (Test-Path -LiteralPath $sourceModule -PathType Leaf)) {
    throw "Missing source module: $sourceModule"
}
if (-not (Test-Path -LiteralPath $esbuild -PathType Leaf)) {
    throw "esbuild not found at $esbuild -- run 'npm install' in ui/ first (esbuild ships with Vite, no extra dependency needed)."
}

$tempDir = Join-Path ([System.IO.Path]::GetTempPath()) ("killengine_workspace_import_validation_test_" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $tempDir -Force | Out-Null
$transpiled = Join-Path $tempDir "workspaceImportValidation.mjs"

try {
    Write-Host "[RUN] Transpile workspaceImportValidation.ts" -ForegroundColor Cyan
    & $esbuild $sourceModule --format=esm --outfile=$transpiled
    if ($LASTEXITCODE -ne 0) {
        throw "esbuild failed with code $LASTEXITCODE"
    }
    if (-not (Test-Path -LiteralPath $transpiled -PathType Leaf)) {
        throw "esbuild did not produce $transpiled"
    }
    Write-Host "[OK]  Transpile workspaceImportValidation.ts" -ForegroundColor Green

    Write-Host "[RUN] Workspace import validation assertions" -ForegroundColor Cyan
    node $testScript $transpiled
    if ($LASTEXITCODE -ne 0) {
        throw "Workspace import validation assertions failed (node exit code $LASTEXITCODE)"
    }
    Write-Host "[OK]  Workspace import validation assertions" -ForegroundColor Green
} finally {
    Remove-Item -LiteralPath $tempDir -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host "Workspace import validation complete." -ForegroundColor Green
