# Automated tests for the tutorial guide verification predicates
# (ui/src/stores/tutorialSteps.ts, UX-PRODUIT-15C): convergence toward the
# health address while decoys are excluded, write-result targeting, effect
# verification, and profile-entry matching.
#
# ui/ has no JS/TS test runner configured. Rather than reimplement the
# predicates a second time in the test (drift risk) or add a new test
# framework dependency, this transpiles the real .ts module with the esbuild
# binary already vendored by Vite (ui/node_modules/.bin/esbuild) and runs the
# transpiled module directly with node -- same pattern as
# scripts/test-trainer-dependencies.ps1.
#
# Usage:
#   .\scripts\test-tutorial-steps.ps1

$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$sourceModule = Join-Path $repoRoot "ui\src\stores\tutorialSteps.ts"
$testScript = Join-Path $repoRoot "scripts\test-tutorial-steps.mjs"
$esbuild = Join-Path $repoRoot "ui\node_modules\.bin\esbuild.cmd"

if (-not (Test-Path -LiteralPath $sourceModule -PathType Leaf)) {
    throw "Missing source module: $sourceModule"
}
if (-not (Test-Path -LiteralPath $esbuild -PathType Leaf)) {
    throw "esbuild not found at $esbuild -- run 'npm install' in ui/ first (esbuild ships with Vite, no extra dependency needed)."
}

$tempDir = Join-Path ([System.IO.Path]::GetTempPath()) ("killengine_tutorial_steps_test_" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $tempDir -Force | Out-Null
$transpiled = Join-Path $tempDir "tutorialSteps.mjs"

try {
    Write-Host "[RUN] Transpile tutorialSteps.ts" -ForegroundColor Cyan
    & $esbuild $sourceModule --format=esm --outfile=$transpiled
    if ($LASTEXITCODE -ne 0) {
        throw "esbuild failed with code $LASTEXITCODE"
    }
    if (-not (Test-Path -LiteralPath $transpiled -PathType Leaf)) {
        throw "esbuild did not produce $transpiled"
    }
    Write-Host "[OK]  Transpile tutorialSteps.ts" -ForegroundColor Green

    Write-Host "[RUN] Tutorial step assertions" -ForegroundColor Cyan
    node $testScript $transpiled
    if ($LASTEXITCODE -ne 0) {
        throw "Tutorial step assertions failed (node exit code $LASTEXITCODE)"
    }
    Write-Host "[OK]  Tutorial step assertions" -ForegroundColor Green
} finally {
    Remove-Item -LiteralPath $tempDir -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host "Tutorial step validation complete." -ForegroundColor Green
