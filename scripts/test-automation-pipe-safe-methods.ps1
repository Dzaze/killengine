<#
Batterie de non-regression du pipe d'automatisation (docs/PHASE_TRACKER.md
PHASE 127 : "le pipe d'automatisation est le canal de controle universel du
moteur, et ca ne doit pas rester vrai en theorie par reflexion seulement").

Objectif : verifier automatiquement, contre un vrai KillEngine.exe +
KillEngineTestTarget.exe, qu'un lot cure de methodes Q_INVOKABLE "safe"
(lecture seule, aucune ecriture memoire, aucun clic UI requis) restent
appelables et repondent avec la forme attendue. Ne remplace pas un smoke
test manuel complet -- couvre juste la regression "cette methode a cesse de
repondre / a change de forme" sans intervention humaine.

Methodes DELIBEREMENT EXCLUES de cette batterie (avec la raison) :
  - write_value/freeze_value/applyCodePatch/restoreCodePatch/kernel_write/
    patch_file_bytes/speedhack_set/block_process_network : ecrivent la
    memoire, le disque ou l'etat reseau/CPU de la cible -- une regression
    automatisee qui tourne sans supervision ne doit jamais faire ca.
  - findWhatWrites/findWhatAccesses/findWhatExecutes/analyzeFieldStability :
    attachent un debugger Win32 (DebugActiveProcess) a la cible. Documente
    (AGENTS.md, section "Find What Writes") : peut crasher une cible "chaude"
    (adresse reecrite tres frequemment) -- deja reproduit en session reelle
    (docs/PHASE_TRACKER.md PHASE 127, capture sur g_health sous stress
    rewrite). Pas adapte a une regression automatisee non supervisee ;
    couvert par ailleurs par des tests d'integration dedies
    (tests/integration/test_power_up_runtime.cpp,
    test_display_vs_source_target.cpp) qui choisissent une adresse connue
    pour rester sures.
  - executeLuaScript (synchrone) : bloque le thread qui doit justement
    servir une connexion pipe imbriquee (PHASE 127) -- hors sujet ici (pas
    une methode a tester isolement, un piege deja documente ailleurs).
  - trainer_apply_request/trainer_restore_request (Assistant) et
    applyTrainerFeature/restoreTrainerFeature (callVueStoreAction) :
    declenchent un vrai confirmRiskAction modal cote frontend (PHASE 121) --
    dependent d'un clic humain, pas automatisables sans supervision.
  - injectDllIntoProcess/installInlineHook/startSpeedhack : injection/hooking
    in-process, effets de bord sur la cible au-dela d'une simple lecture.

Usage :
  .\scripts\test-automation-pipe-safe-methods.ps1
  .\scripts\test-automation-pipe-safe-methods.ps1 -KillEnginePath build\bin\KillEngine.exe -TargetPath build\bin\KillEngineTestTarget.exe
#>
param(
    [string]$KillEnginePath,
    [string]$TargetPath,
    [string]$PipeName = 'KillEngineAutomationPipe',
    [int]$PipeTimeoutMs = 5000
)

$ErrorActionPreference = 'Stop'

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if (-not $KillEnginePath) { $KillEnginePath = Join-Path $repoRoot 'build\bin\KillEngine.exe' }
if (-not $TargetPath) { $TargetPath = Join-Path $repoRoot 'build\bin\KillEngineTestTarget.exe' }
$pipeCall = Join-Path $repoRoot 'scripts\automation-pipe-call.ps1'

if (-not (Test-Path -LiteralPath $KillEnginePath -PathType Leaf)) { throw "KillEngine.exe introuvable: $KillEnginePath" }
if (-not (Test-Path -LiteralPath $TargetPath -PathType Leaf)) { throw "KillEngineTestTarget.exe introuvable: $TargetPath" }

function Invoke-Pipe {
    param([string]$Method, [string]$ParamsJson = '[]')
    $raw = & $pipeCall -Method $Method -ParamsJson $ParamsJson -PipeName $PipeName -TimeoutMs $PipeTimeoutMs
    return $raw | ConvertFrom-Json
}

$results = New-Object System.Collections.Generic.List[object]
function Test-Case {
    param([string]$Name, [scriptblock]$Body)
    Write-Host "[RUN] $Name" -ForegroundColor Cyan
    try {
        & $Body
        Write-Host "[OK]  $Name" -ForegroundColor Green
        $results.Add([pscustomobject]@{ Name = $Name; Passed = $true; Error = '' })
    } catch {
        Write-Host "[FAIL] $Name -- $_" -ForegroundColor Red
        $results.Add([pscustomobject]@{ Name = $Name; Passed = $false; Error = $_.ToString() })
    }
}

$targetProc = $null
$keProc = $null

try {
    $targetProc = Start-Process -FilePath $TargetPath -PassThru
    Start-Sleep -Milliseconds 800
    if ($targetProc.HasExited) { throw "KillEngineTestTarget.exe a quitte immediatement (exit code $($targetProc.ExitCode))." }

    $env:KILLENGINE_AUTOMATION_PIPE = '1'
    $keProc = Start-Process -FilePath $KillEnginePath -PassThru
    Remove-Item Env:\KILLENGINE_AUTOMATION_PIPE
    Start-Sleep -Seconds 3
    if ($keProc.HasExited) { throw "KillEngine.exe a quitte immediatement (exit code $($keProc.ExitCode)) -- une autre instance tenait deja le pipe ?" }

    $markerFile = Join-Path $env:TEMP 'killengine_test_target_addresses.txt'
    $healthAddress = $null
    if (Test-Path -LiteralPath $markerFile) {
        $markerLines = Get-Content -LiteralPath $markerFile
        foreach ($line in $markerLines) {
            if ($line -match '^g_health=(0x[0-9a-fA-F]+)$') { $healthAddress = $Matches[1] }
        }
    }

    Test-Case 'ping repond avec pong + le message envoye' {
        $r = Invoke-Pipe -Method 'ping' -ParamsJson '["killengine-safe-battery"]'
        if ($r.result -notlike 'pong:*killengine-safe-battery*') { throw "attendu 'pong: ...killengine-safe-battery...', recu: $($r.result)" }
    }

    Test-Case 'getVersion retourne une chaine non vide' {
        $r = Invoke-Pipe -Method 'getVersion'
        if ([string]::IsNullOrWhiteSpace($r.result)) { throw 'version vide' }
    }

    Test-Case 'getSettings retourne un objet' {
        $r = Invoke-Pipe -Method 'getSettings'
        if ($null -eq $r.result) { throw 'result null' }
    }

    Test-Case 'getLuaScriptingStatus retourne success=true' {
        $r = Invoke-Pipe -Method 'getLuaScriptingStatus'
        if ($r.result.success -ne $true) { throw "success attendu true, recu $($r.result.success)" }
    }

    Test-Case 'getProcesses liste bien le process cible' {
        $r = Invoke-Pipe -Method 'getProcesses'
        $match = $r.result | Where-Object { $_.pid -eq $targetProc.Id }
        if (-not $match) { throw "PID $($targetProc.Id) absent de getProcesses()" }
    }

    Test-Case 'attachProcess reussit sur la cible de test' {
        $r = Invoke-Pipe -Method 'attachProcess' -ParamsJson "[$($targetProc.Id)]"
        if ($r.result -ne $true) { throw "attachProcess a retourne $($r.result)" }
    }

    if ($healthAddress) {
        Test-Case 'readMemoryPreview lit g_health (4 octets)' {
            $r = Invoke-Pipe -Method 'readMemoryPreview' -ParamsJson "[`"$healthAddress`", 4]"
            if ($r.result.success -ne $true) { throw "success attendu true, recu $($r.result.success) (error: $($r.result.error))" }
            if ($r.result.bytesRead -ne 4) { throw "bytesRead attendu 4, recu $($r.result.bytesRead)" }
        }
    } else {
        Write-Host "[SKIP] readMemoryPreview -- fichier marqueur $markerFile introuvable ou incomplet" -ForegroundColor Yellow
    }

    Test-Case 'discoverProcessSaveFiles repond sans erreur reseau/IO' {
        $r = Invoke-Pipe -Method 'discoverProcessSaveFiles' -ParamsJson '[10]'
        if ($null -eq $r.result) { throw 'result null' }
        # KillEngineTestTarget.exe n'est pas un package UWP : succes=false est
        # une reponse VALIDE ici (pas de LocalState), seul un crash/timeout du
        # pipe est un vrai echec de regression.
        if (-not ($r.result.PSObject.Properties.Name -contains 'success')) { throw "reponse sans champ success: $($r | ConvertTo-Json -Compress)" }
    }

    Test-Case 'getBreakpointFreezeStats repond sans freeze actif' {
        $r = Invoke-Pipe -Method 'getBreakpointFreezeStats'
        if ($null -eq $r.result) { throw 'result null' }
    }

    Test-Case 'cancelFindWhatWrites repond proprement sans capture active' {
        $r = Invoke-Pipe -Method 'cancelFindWhatWrites'
        if ($null -eq $r.result) { throw 'result null' }
    }

    Test-Case 'callVueStoreAction(getTrainerFeaturesSnapshot) atteint le store Pinia' {
        $r = Invoke-Pipe -Method 'callVueStoreAction' -ParamsJson '["getTrainerFeaturesSnapshot", []]'
        if ($r.result.success -ne $true) { throw "success attendu true, recu $($r.result.success) (error: $($r.result.error)) -- la page UI a-t-elle fini de charger ?" }
    }

    Test-Case 'detachProcess ne leve pas d''erreur (retourne un bool depuis AM-2, 16/09/2026)' {
        Invoke-Pipe -Method 'detachProcess' | Out-Null
    }
} finally {
    if ($keProc -and -not $keProc.HasExited) { Stop-Process -Id $keProc.Id -Force -ErrorAction SilentlyContinue }
    if ($targetProc -and -not $targetProc.HasExited) { Stop-Process -Id $targetProc.Id -Force -ErrorAction SilentlyContinue }
}

$passed = ($results | Where-Object { $_.Passed }).Count
$failed = ($results | Where-Object { -not $_.Passed }).Count
Write-Host ""
Write-Host "Pipe safe-methods battery: $passed/$($results.Count) OK" -ForegroundColor $(if ($failed -eq 0) { 'Green' } else { 'Red' })
if ($failed -gt 0) {
    Write-Host "Echecs :" -ForegroundColor Red
    $results | Where-Object { -not $_.Passed } | ForEach-Object { Write-Host "  - $($_.Name): $($_.Error)" -ForegroundColor Red }
    exit 1
}
exit 0
