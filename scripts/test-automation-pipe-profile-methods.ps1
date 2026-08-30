<#
Smoke test cible du cluster profils (apps/desktop/profile_manager.h/.cpp,
extrait de ApplicationController en PHASE 231, docs/PHASE_TRACKER.md) via le
pipe d'automatisation (docs/AUTOMATION_API.md). Complement de
test-automation-pipe-safe-methods.ps1 : celui-la ne teste QUE des methodes en
lecture seule, hors sujet pour verifier un cycle profil complet puisque
saveProfileTarget/deleteProfile ecrivent/suppriment un vrai fichier
.keprofile. Reste "safe" au sens ou :
  - aucune ecriture memoire dans le processus cible (contrairement a
    write_value/freeze_value, deliberement exclus de la batterie safe-methods
    pour cette raison),
  - le seul effet de bord disque est le profil de test cree PUIS supprime par
    ce meme script (deleteProfile en fin de run), dans un bloc try/finally.

Methodes du cluster profils DELIBEREMENT NON couvertes ici (perimetre plus
large, a tester separement si besoin) :
  - saveClrFieldProfileTarget/activateProfileTarget : necessitent un objet CLR
    reel cote cible (KillEngineClrTestTarget.exe, pas KillEngineTestTarget.exe).
  - save/apply/restore/inspectProfileCodePatches : ecrivent des bytes de code
    dans la cible (meme categorie de risque que applyCodePatch, deja exclu
    ailleurs).
  - save/apply/deleteProfileAutoAsmScript, save/deleteProfileLuaScript :
    perimetre scripting, pas le coeur "profil = cibles memoire" teste ici.
  - scanPointerChains/resolvePointerChain/suggestStableLocatorForAddress :
    scans potentiellement longs, pas adaptes a une regression rapide non
    supervisee (deja la logique suivie par safe-methods pour findWhatWrites).
  - exportPointerMap/importPointerMap/exportGhidraArtifacts/
    importGhidraSymbols/setProfileTargetDependencies : pas sur le chemin
    save/load/resolve/compare/delete le plus courant, a couvrir separement.

Usage :
  .\scripts\test-automation-pipe-profile-methods.ps1
  .\scripts\test-automation-pipe-profile-methods.ps1 -KillEnginePath build\bin\KillEngine.exe -TargetPath build\bin\KillEngineTestTarget.exe
#>
param(
    [string]$KillEnginePath,
    [string]$TargetPath,
    [string]$PipeName = 'KillEngineAutomationPipe',
    [int]$PipeTimeoutMs = 5000,
    [string]$ProfileTestName = 'KillEngineAutomationPipeSmokeTest'
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

function Normalize-HexAddress {
    param([string]$Value)
    $clean = $Value.Trim()
    if ($clean.StartsWith('0x', [StringComparison]::OrdinalIgnoreCase)) { $clean = $clean.Substring(2) }
    $clean = $clean.ToLowerInvariant().TrimStart('0')
    if ($clean -eq '') { $clean = '0' }
    return $clean
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
    if (-not (Test-Path -LiteralPath $markerFile)) { throw "Fichier marqueur introuvable: $markerFile" }
    $healthAddress = $null
    foreach ($line in Get-Content -LiteralPath $markerFile) {
        if ($line -match '^g_health=(0x[0-9a-fA-F]+)$') { $healthAddress = $Matches[1] }
    }
    if (-not $healthAddress) { throw "g_health absent de $markerFile" }
    $healthAddressNormalized = Normalize-HexAddress $healthAddress
    Write-Host "g_health = $healthAddress" -ForegroundColor DarkGray

    Test-Case 'attachProcess reussit sur la cible de test' {
        $r = Invoke-Pipe -Method 'attachProcess' -ParamsJson "[$($targetProc.Id)]"
        if ($r.result -ne $true) { throw "attachProcess a retourne $($r.result)" }
    }

    Test-Case 'saveProfileTarget sauvegarde HealthSmoke (locator module_offset)' {
        $paramsJson = "[`"$ProfileTestName`", `"HealthSmoke`", `"$healthAddress`", `"Int32`", `"Smoke test pipe d'automatisation (test-automation-pipe-profile-methods.ps1)`"]"
        $r = Invoke-Pipe -Method 'saveProfileTarget' -ParamsJson $paramsJson
        if ($r.result.success -ne $true) { throw "success attendu true, recu $($r.result.success) (error: $($r.result.error))" }
        if ($r.result.targetName -ne 'HealthSmoke') { throw "targetName attendu 'HealthSmoke', recu $($r.result.targetName)" }
        if ($r.result.locatorKind -ne 'module_offset') { throw "locatorKind attendu 'module_offset', recu $($r.result.locatorKind)" }
    }

    Test-Case 'loadProfile retrouve la cible HealthSmoke' {
        $r = Invoke-Pipe -Method 'loadProfile' -ParamsJson "[`"$ProfileTestName`"]"
        if ($r.result.success -ne $true) { throw "success attendu true, recu $($r.result.success) (error: $($r.result.error))" }
        $match = $r.result.targets | Where-Object { $_.name -eq 'HealthSmoke' }
        if (-not $match) { throw "cible HealthSmoke absente de loadProfile.targets ($($r.result.targets | ConvertTo-Json -Compress))" }
    }

    Test-Case 'resolveProfileTarget resout HealthSmoke exactement sur g_health' {
        $r = Invoke-Pipe -Method 'resolveProfileTarget' -ParamsJson "[`"$ProfileTestName`", `"HealthSmoke`"]"
        if ($r.result.success -ne $true) { throw "success attendu true, recu $($r.result.success) (error: $($r.result.error))" }
        $resolvedNormalized = Normalize-HexAddress ([string]$r.result.address)
        if ($resolvedNormalized -ne $healthAddressNormalized) {
            throw "adresse resolue '$($r.result.address)' != g_health attendu '$healthAddress'"
        }
    }

    Test-Case 'comparePointerMapAcrossRestart repond avec un status valid' {
        $r = Invoke-Pipe -Method 'comparePointerMapAcrossRestart' -ParamsJson "[`"$ProfileTestName`"]"
        if ($null -eq $r.result) { throw 'result null' }
        if ($r.result.success -ne $true) { throw "success attendu true, recu $($r.result.success) (error: $($r.result.error))" }
        # Champ "results", pas "entries" -- voir docs/AUTOMATION_API.md (piege deja fait une fois, PHASE 231).
        $entry = $r.result.results | Where-Object { $_.targetName -eq 'HealthSmoke' }
        if (-not $entry) { throw "entree HealthSmoke absente de comparePointerMapAcrossRestart.results" }
        if ($entry.status -ne 'valid') { throw "status attendu 'valid' pour HealthSmoke, recu $($entry.status)" }
    }

    Test-Case 'deleteProfile nettoie le profil de test' {
        $r = Invoke-Pipe -Method 'deleteProfile' -ParamsJson "[`"$ProfileTestName`"]"
        if ($r.result -ne $true) { throw "deleteProfile a retourne $($r.result)" }
    }

    Test-Case 'loadProfile confirme la suppression' {
        $r = Invoke-Pipe -Method 'loadProfile' -ParamsJson "[`"$ProfileTestName`"]"
        if ($r.result.success -ne $false) { throw "success attendu false apres suppression, recu $($r.result.success)" }
    }

    Test-Case 'detachProcess ne leve pas d''erreur (retour void)' {
        Invoke-Pipe -Method 'detachProcess' | Out-Null
    }
} finally {
    # Filet de securite en plus de la Test-Case deleteProfile ci-dessus : si un
    # throw anterieur a saute cette etape, ne pas laisser le profil de test
    # trainer sur le disque entre deux runs.
    try {
        if ($keProc -and -not $keProc.HasExited) {
            Invoke-Pipe -Method 'deleteProfile' -ParamsJson "[`"$ProfileTestName`"]" | Out-Null
        }
    } catch {
        # Best-effort : si le pipe est deja mort, tant pis, rien a nettoyer par ce biais.
    }
    if ($keProc -and -not $keProc.HasExited) { Stop-Process -Id $keProc.Id -Force -ErrorAction SilentlyContinue }
    if ($targetProc -and -not $targetProc.HasExited) { Stop-Process -Id $targetProc.Id -Force -ErrorAction SilentlyContinue }
}

$passed = ($results | Where-Object { $_.Passed }).Count
$failed = ($results | Where-Object { -not $_.Passed }).Count
Write-Host ""
Write-Host "Automation pipe profile-methods battery: $passed/$($results.Count) OK" -ForegroundColor $(if ($failed -eq 0) { 'Green' } else { 'Red' })
if ($failed -gt 0) {
    Write-Host "Echecs :" -ForegroundColor Red
    $results | Where-Object { -not $_.Passed } | ForEach-Object { Write-Host "  - $($_.Name): $($_.Error)" -ForegroundColor Red }
    exit 1
}
exit 0
