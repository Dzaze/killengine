# UX-PRODUIT-14B -- tests deterministes/simules de la garde anti-course
# requestId dans BackendService.startExactScanAsync (ui/src/services/
# backend.ts). Ne touche a AUCUNE instance KillEngine.exe reelle : prouve la
# logique de correlation/desabonnement elle-meme (resultat immediat avant
# resolution du start, erreur de demarrage, signal perime ignore, signal
# duplique resolu une seule fois), pas le comportement Win32 reel -- voir
# scripts/test-behavior-manifest.ps1 pour la preuve live complementaire
# (deux preuves distinctes, jamais confondues, exigence explicite de la
# fiche UX-PRODUIT-14B).
#
# backend.ts importe `@/i18n` (alias Vite non resolvable par un esbuild nu
# hors Vite) uniquement pour un message d'erreur de timeout -- sans rapport
# avec la logique testee ici. Ce script copie backend.ts dans un fichier
# temporaire avec cet import remplace par un stub minimal (meme forme
# `.global.t`), transpile CETTE copie (le reste du code reel, inchange), puis
# l'execute avec node. Le code de garde reellement teste est donc le vrai
# code source, pas une reimplementation.
#
# Usage:
#   .\scripts\test-scan-race-guards.ps1

$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$sourceModule = Join-Path $repoRoot "ui\src\services\backend.ts"
$testScript = Join-Path $repoRoot "scripts\test-scan-race-guards.mjs"
$esbuild = Join-Path $repoRoot "ui\node_modules\.bin\esbuild.cmd"

if (-not (Test-Path -LiteralPath $sourceModule -PathType Leaf)) {
    throw "Missing source module: $sourceModule"
}
if (-not (Test-Path -LiteralPath $esbuild -PathType Leaf)) {
    throw "esbuild not found at $esbuild -- run 'npm install' in ui/ first (esbuild ships with Vite, no extra dependency needed)."
}

$tempDir = Join-Path ([System.IO.Path]::GetTempPath()) ("killengine_scan_race_test_" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $tempDir -Force | Out-Null
$patchedSource = Join-Path $tempDir "backend_patched.ts"
$transpiled = Join-Path $tempDir "backend_patched.mjs"

try {
    Write-Host "[RUN] Prepare a Node-safe copy of backend.ts (stub @/i18n import only)" -ForegroundColor Cyan
    $content = Get-Content -LiteralPath $sourceModule -Raw -Encoding utf8
    $needle = "import { i18n } from '@/i18n'"
    if ($content -notmatch [regex]::Escape($needle)) {
        throw "Expected import line not found in backend.ts -- this script's assumption about the file's top may be stale, update the needle."
    }
    $replacement = "const i18n = { global: { t: (key) => key } } // stub injecte par test-scan-race-guards.ps1, voir commentaire d'en-tete"
    $patchedContent = $content -replace [regex]::Escape($needle), $replacement
    Set-Content -Path $patchedSource -Value $patchedContent -Encoding utf8NoBOM -NoNewline
    Write-Host "[OK]  Node-safe copy prepared" -ForegroundColor Green

    Write-Host "[RUN] Transpile the patched backend.ts" -ForegroundColor Cyan
    & $esbuild $patchedSource --format=esm --outfile=$transpiled
    if ($LASTEXITCODE -ne 0) {
        throw "esbuild failed with code $LASTEXITCODE"
    }
    if (-not (Test-Path -LiteralPath $transpiled -PathType Leaf)) {
        throw "esbuild did not produce $transpiled"
    }
    Write-Host "[OK]  Transpile the patched backend.ts" -ForegroundColor Green

    Write-Host "[RUN] Scan race-guard assertions" -ForegroundColor Cyan
    node $testScript $transpiled
    if ($LASTEXITCODE -ne 0) {
        throw "Scan race-guard assertions failed (node exit code $LASTEXITCODE)"
    }
    Write-Host "[OK]  Scan race-guard assertions" -ForegroundColor Green
} finally {
    Remove-Item -LiteralPath $tempDir -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host "Scan race-guard validation complete." -ForegroundColor Green
