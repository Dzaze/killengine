# Automated tests for the behavior-manifest field checker
# (scripts/lib/behavior-manifest-check.mjs, UX-PRODUIT-14B). Pure JS module,
# no TypeScript/esbuild transpile step needed -- run directly with node.
#
# Usage:
#   .\scripts\test-behavior-manifest-check.ps1

$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$testScript = Join-Path $repoRoot "scripts\test-behavior-manifest-check.mjs"

if (-not (Test-Path -LiteralPath $testScript -PathType Leaf)) {
    throw "Missing test script: $testScript"
}

Write-Host "[RUN] Behavior manifest checker assertions" -ForegroundColor Cyan
node $testScript
if ($LASTEXITCODE -ne 0) {
    throw "Behavior manifest checker assertions failed (node exit code $LASTEXITCODE)"
}
Write-Host "[OK]  Behavior manifest checker assertions" -ForegroundColor Green
