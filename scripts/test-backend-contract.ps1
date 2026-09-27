# UX-PRODUIT-14A (docs/PHASE_TRACKER.md) -- controle de contrat backend :
# inventaire structurel reel du controleur (ApplicationController::
# describeBackendContract(), via le pipe d'automatisation) compare a
# l'interface TypeScript BackendController et au mock (createMockBackend),
# en reutilisant l'API compilateur TypeScript deja vendoree pour vue-tsc
# (scripts/lib/backend-contract-ts.mjs) plutot que des regex fragiles.
#
# Isolation (fiche point 6, meme esprit que test-ui-journeys.ps1) : ce script
# ne touche JAMAIS une instance KillEngine preexistante. Si une instance
# tourne deja, il tente juste un appel pipe direct (SKIPPED proprement si le
# pipe ne repond pas) sans jamais en lancer une seconde en parallele. Si
# aucune instance ne tourne, il en lance une temporaire dediee avec le pipe
# active, puis la termine explicitement par PID (cleanup des SEULS processus
# qu'il a lui-meme crees).
#
# Usage:
#   .\scripts\test-backend-contract.ps1
#   .\scripts\test-backend-contract.ps1 -SkipAutomationPipe   # controle statique TS seul

param(
    [switch]$SkipAutomationPipe
)

$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$backendTsPath = Join-Path $repoRoot "ui\src\services\backend.ts"
$orchestrator = Join-Path $repoRoot "scripts\test-backend-contract.mjs"
$pipeCallScript = Join-Path $repoRoot "scripts\automation-pipe-call.ps1"
$killEngineExe = Join-Path $repoRoot "build\bin\KillEngine.exe"

if (-not (Test-Path -LiteralPath $backendTsPath -PathType Leaf)) {
    throw "Fichier introuvable : $backendTsPath"
}

$cppContractPath = $null
$startedOwnInstance = $false
$ownProcess = $null

if ($SkipAutomationPipe) {
    Write-Host "[SKIP] Inventaire C++ runtime (-SkipAutomationPipe demande) -- controle statique TS seul." -ForegroundColor Yellow
} else {
    $cppContractPath = Join-Path ([System.IO.Path]::GetTempPath()) ("killengine_backend_contract_" + [Guid]::NewGuid().ToString("N") + ".json")

    $existing = @(Get-Process -Name "KillEngine" -ErrorAction SilentlyContinue)
    if ($existing.Count -gt 0) {
        Write-Host "[INFO] Instance KillEngine deja en cours (PID $($existing[0].Id)) -- tentative d'appel pipe direct, aucune seconde instance ne sera lancee." -ForegroundColor Cyan
        try {
            & $pipeCallScript -Method describeBackendContract -ParamsJson '[]' -OutFile $cppContractPath 2>$null | Out-Null
            # automation-pipe-call.ps1 ne positionne pas $LASTEXITCODE de facon fiable
            # (confirme en debug direct : reste $null meme en cas de succes) -- seule
            # la presence d'un fichier de sortie non vide fait foi ici.
            if (-not (Test-Path -LiteralPath $cppContractPath) -or (Get-Item -LiteralPath $cppContractPath).Length -eq 0) {
                throw "pipe call produced no output"
            }
        } catch {
            Write-Host "[SKIP] Pipe indisponible sur l'instance existante (pas de KILLENGINE_AUTOMATION_PIPE=1 ?) -- controle statique TS seul." -ForegroundColor Yellow
            $cppContractPath = $null
        }
    } elseif (-not (Test-Path -LiteralPath $killEngineExe -PathType Leaf)) {
        Write-Host "[SKIP] build\bin\KillEngine.exe introuvable (pas encore buildé ?) -- controle statique TS seul." -ForegroundColor Yellow
        $cppContractPath = $null
    } else {
        Write-Host "[RUN] Lancement d'une instance KillEngine temporaire dediee (pipe active)..." -ForegroundColor Cyan
        $env:KILLENGINE_AUTOMATION_PIPE = "1"
        $ownProcess = Start-Process -FilePath $killEngineExe -PassThru
        $startedOwnInstance = $true
        try {
            $deadline = (Get-Date).AddSeconds(20)
            $ready = $false
            while ((Get-Date) -lt $deadline) {
                Start-Sleep -Milliseconds 500
                try {
                    & $pipeCallScript -Method describeBackendContract -ParamsJson '[]' -OutFile $cppContractPath 2>$null | Out-Null
                    if ((Test-Path -LiteralPath $cppContractPath) -and (Get-Item -LiteralPath $cppContractPath).Length -gt 0) {
                        $ready = $true
                        break
                    }
                } catch {
                    # pas encore pret, on reessaie jusqu'au timeout
                }
            }
            if (-not $ready) {
                Write-Host "[SKIP] L'instance temporaire n'a pas repondu sur le pipe dans le delai -- controle statique TS seul." -ForegroundColor Yellow
                $cppContractPath = $null
            } else {
                Write-Host "[OK]  Inventaire C++ recupere depuis l'instance temporaire." -ForegroundColor Green
            }
        } finally {
            if ($startedOwnInstance -and $ownProcess -and -not $ownProcess.HasExited) {
                Write-Host "[INFO] Arret de l'instance KillEngine temporaire (PID $($ownProcess.Id))." -ForegroundColor Cyan
                Stop-Process -Id $ownProcess.Id -Force -ErrorAction SilentlyContinue
            }
        }
    }
}

try {
    $nodeArgs = @($orchestrator, $backendTsPath)
    if ($cppContractPath) { $nodeArgs += $cppContractPath }
    & node @nodeArgs
    $exitCode = $LASTEXITCODE
} finally {
    if ($cppContractPath -and (Test-Path -LiteralPath $cppContractPath)) {
        Remove-Item -LiteralPath $cppContractPath -Force -ErrorAction SilentlyContinue
    }
}

exit $exitCode
