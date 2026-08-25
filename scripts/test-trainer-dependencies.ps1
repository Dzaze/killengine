# Automated tests for the Trainer dependsOn logic (ui/src/stores/trainerDependencies.ts),
# covering what PHASE 103/111/112/114 validated live/manually: Apply all order,
# Restore all reversed order, a togglable action (freeze_polling, not just
# one-shot write), delete cleanup of dead references, cycle/missing-reference
# detection.
#
# ui/ has no JS/TS test runner configured. Rather than reimplement the
# algorithm a second time in the test (drift risk) or add a new test
# framework dependency, this transpiles the real .ts module with the esbuild
# binary already vendored by Vite (ui/node_modules/.bin/esbuild) and runs the
# transpiled module directly with node.
#
# Usage:
#   .\scripts\test-trainer-dependencies.ps1

$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$sourceModule = Join-Path $repoRoot "ui\src\stores\trainerDependencies.ts"
$testScript = Join-Path $repoRoot "scripts\test-trainer-dependencies.mjs"
$esbuild = Join-Path $repoRoot "ui\node_modules\.bin\esbuild.cmd"

if (-not (Test-Path -LiteralPath $sourceModule -PathType Leaf)) {
    throw "Missing source module: $sourceModule"
}
if (-not (Test-Path -LiteralPath $esbuild -PathType Leaf)) {
    throw "esbuild not found at $esbuild -- run 'npm install' in ui/ first (esbuild ships with Vite, no extra dependency needed)."
}

$tempDir = Join-Path ([System.IO.Path]::GetTempPath()) ("killengine_trainer_deps_test_" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $tempDir -Force | Out-Null
$transpiled = Join-Path $tempDir "trainerDependencies.mjs"

try {
    Write-Host "[RUN] Transpile trainerDependencies.ts" -ForegroundColor Cyan
    & $esbuild $sourceModule --format=esm --outfile=$transpiled
    if ($LASTEXITCODE -ne 0) {
        throw "esbuild failed with code $LASTEXITCODE"
    }
    if (-not (Test-Path -LiteralPath $transpiled -PathType Leaf)) {
        throw "esbuild did not produce $transpiled"
    }
    Write-Host "[OK]  Transpile trainerDependencies.ts" -ForegroundColor Green

    Write-Host "[RUN] Trainer dependency assertions" -ForegroundColor Cyan
    node $testScript $transpiled
    if ($LASTEXITCODE -ne 0) {
        throw "Trainer dependency assertions failed (node exit code $LASTEXITCODE)"
    }
    Write-Host "[OK]  Trainer dependency assertions" -ForegroundColor Green
} finally {
    Remove-Item -LiteralPath $tempDir -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host "Trainer dependency validation complete." -ForegroundColor Green
