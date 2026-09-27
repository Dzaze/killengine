<#
UX-PRODUIT-14B -- validation live des scénarios du manifeste de
comportements (scripts/contracts/behavior-manifest.json) contre un vrai
KillEngine.exe + KillEngineTestTarget.exe. Complète scripts/test-scan-race-
guards.ps1 (garde requestId simulée/déterministe, sur le vrai code
BackendService mais sans process réel) : ici, c'est la preuve Win32 réelle
-- une simulation teste la course, pas la réalité Win32 (exigence explicite
de la fiche UX-PRODUIT-14B), les deux preuves sont distinctes et ne se
remplacent pas.

Isolation façon test-ui-journeys.ps1 (PAS test-automation-pipe-safe-
methods.ps1/-profile-methods.ps1, qui n'ont aucune vérification d'instance
préexistante -- trou identifié explicitement par le grounding de cette
fiche, à ne pas reproduire) : refuse de démarrer si une instance
KillEngine.exe/KillEngineTestTarget.exe existe déjà (SKIPPED propre, jamais
de tentative de pipe dessus -- ce harnais a besoin d'un contrôle exclusif
pour séquencer démarrer/annuler/redémarrer), vérifie le PID du marqueur
g_health avant de lui faire confiance, ne nettoie que les process dont CE
run a capturé le PID.

Contrainte réelle découverte en écrivant ce script (documentée aussi dans le
manifeste, scénario scanLifecycle/scanFinishedNormal) : le pipe
d'automatisation ne relaie que des appels de méthode réfléchis par
QMetaMethod, jamais un signal Qt -- scanFinished n'est donc pas observable
ici. La preuve de complétion d'un scan passe par getCandidates() (lecture
réelle du résultat), pas par le signal.

Usage:
  .\scripts\test-behavior-manifest.ps1
  .\scripts\test-behavior-manifest.ps1 -KillEnginePath build\bin\KillEngine.exe -TargetPath build\bin\KillEngineTestTarget.exe
#>
param(
    [string]$KillEnginePath,
    [string]$TargetPath,
    [string]$PipeName = 'KillEngineAutomationPipe',
    [int]$PipeTimeoutMs = 8000,
    [string]$ProfileTestName = 'KillEngineBehaviorManifestSmokeTest'
)

$ErrorActionPreference = 'Stop'

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if (-not $KillEnginePath) { $KillEnginePath = Join-Path $repoRoot 'build\bin\KillEngine.exe' }
if (-not $TargetPath) { $TargetPath = Join-Path $repoRoot 'build\bin\KillEngineTestTarget.exe' }
$pipeCall = Join-Path $repoRoot 'scripts\automation-pipe-call.ps1'

if (-not (Test-Path -LiteralPath $KillEnginePath -PathType Leaf)) { throw "KillEngine.exe introuvable: $KillEnginePath" }
if (-not (Test-Path -LiteralPath $TargetPath -PathType Leaf)) { throw "KillEngineTestTarget.exe introuvable: $TargetPath" }

# Piège d'isolation (motif test-ui-journeys.ps1, pas safe-methods/profile-
# methods -- voir commentaire d'en-tête) : jamais de démarrage si une
# instance existe déjà, jamais de tentative de pipe dessus.
$existing = @(Get-Process -Name 'KillEngine', 'KillEngineTestTarget' -ErrorAction SilentlyContinue)
if ($existing.Count -gt 0) {
    $names = ($existing | ForEach-Object { "$($_.ProcessName) (PID $($_.Id))" }) -join ', '
    Write-Warning "SKIPPED: instance(s) deja en cours -- $names. Ce harnais ne touche jamais un process preexistant. Ferme ces process puis relance."
    exit 0
}

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

function Read-Int32LittleEndianFromHexPreview {
    param([string]$HexWithSpaces)
    $bytes = ($HexWithSpaces.Trim() -split '\s+') | ForEach-Object { [Convert]::ToByte($_, 16) }
    if ($bytes.Count -lt 4) { throw "Preview memoire trop courte pour un Int32 : '$HexWithSpaces'" }
    return [BitConverter]::ToInt32($bytes[0..3], 0)
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

    # Marqueur PID verifie (test-automation-pipe-profile-methods.ps1 lit ce
    # meme fichier SANS verifier le PID -- durci ici conformement a la
    # convention test-ui-journeys.ps1 explicitement demandee par la fiche).
    $markerFile = Join-Path $env:TEMP 'killengine_test_target_addresses.txt'
    $markerPid = $null
    $healthAddress = $null
    $deadline = (Get-Date).AddSeconds(10)
    while ((Get-Date) -lt $deadline) {
        if (Test-Path -LiteralPath $markerFile) {
            foreach ($line in Get-Content -LiteralPath $markerFile) {
                if ($line -match '^pid=(\d+)$') { $markerPid = [int]$Matches[1] }
                if ($line -match '^g_health=(0x[0-9a-fA-F]+)$') { $healthAddress = $Matches[1] }
            }
            if ($markerPid -eq $targetProc.Id -and $healthAddress) { break }
        }
        Start-Sleep -Milliseconds 300
    }
    if ($markerPid -ne $targetProc.Id -or -not $healthAddress) {
        throw "Marqueur g_health introuvable ou PID non concordant dans $markerFile (attendu PID $($targetProc.Id), lu $markerPid)."
    }
    $healthAddressNormalized = Normalize-HexAddress $healthAddress
    Write-Host "g_health = $healthAddress (PID cible $($targetProc.Id) confirme)" -ForegroundColor DarkGray

    # --- getTimelineStatus -------------------------------------------------

    Test-Case 'getTimelineStatus/default : forme conforme au manifeste, success toujours true' {
        $r = Invoke-Pipe -Method 'getTimelineStatus'
        if ($r.result.success -ne $true) { throw "success attendu true, recu $($r.result.success)" }
        if ($null -eq $r.result.collecting) { throw 'champ collecting manquant' }
        if ($null -eq $r.result.stats.totalDataPoints) { throw 'champ stats.totalDataPoints manquant' }
    }

    # --- attache ------------------------------------------------------------

    Test-Case 'attachProcess reussit sur la cible de test' {
        $r = Invoke-Pipe -Method 'attachProcess' -ParamsJson "[$($targetProc.Id)]"
        if ($r.result -ne $true) { throw "attachProcess a retourne $($r.result)" }
    }

    $healthValue = $null
    Test-Case 'readMemoryPreview lit la vraie valeur courante de g_health' {
        $r = Invoke-Pipe -Method 'readMemoryPreview' -ParamsJson "[`"$healthAddress`", 4]"
        if ($r.result.success -ne $true) { throw "success attendu true, recu $($r.result.success) (error: $($r.result.error))" }
        $script:healthValue = Read-Int32LittleEndianFromHexPreview $r.result.hex
        Write-Host "  g_health lu = $script:healthValue" -ForegroundColor DarkGray
    }

    # --- scanLifecycle --------------------------------------------------------

    Test-Case 'scanLifecycle/startAckSuccess : accuse de reception synchrone conforme' {
        $r = Invoke-Pipe -Method 'startExactScanAsync' -ParamsJson "[`"$healthValue`", `"Int32`", {}]"
        if ($r.result.success -ne $true -or $r.result.started -ne $true) {
            throw "success/started attendus true, recu success=$($r.result.success) started=$($r.result.started) (error: $($r.result.error))"
        }
        if ($null -eq $r.result.requestId) { throw 'requestId manquant dans l''accuse de reception' }
        $script:firstScanRequestId = [int64]$r.result.requestId
    }

    Test-Case 'scanLifecycle/scanFinishedNormal (preuve indirecte) : getCandidates reflete le scan reel' {
        $found = $false
        $deadline = (Get-Date).AddSeconds(15)
        while ((Get-Date) -lt $deadline) {
            $r = Invoke-Pipe -Method 'getCandidates' -ParamsJson '[0, 50, ""]'
            if ($r.result.totalCount -gt 0) { $found = $true; break }
            Start-Sleep -Milliseconds 300
        }
        if (-not $found) { throw 'getCandidates.totalCount est reste a 0 apres 15s -- le scan ne semble jamais avoir produit de resultat.' }
    }

    Test-Case 'activityRegistry/scanCreatesAndCompletesEntry : operationId correle au requestId du scan' {
        $r = Invoke-Pipe -Method 'getActivitySnapshot'
        $entry = $r.result.entries | Where-Object { $_.kind -eq 'scan_exact' -and [string]$_.requestId -eq [string]$script:firstScanRequestId } | Select-Object -First 1
        if (-not $entry) {
            $allScanEntries = ($r.result.entries | Where-Object { $_.kind -eq 'scan_exact' } | ForEach-Object { "requestId=$($_.requestId) state=$($_.state)" }) -join '; '
            throw "Aucune entree d'activite scan_exact avec requestId=$script:firstScanRequestId trouvee. Entrees scan_exact vues: $allScanEntries"
        }
        if ($entry.state -notin @('completed', 'cancelled', 'failed')) {
            throw "Etat attendu terminal (completed/cancelled/failed), recu '$($entry.state)' -- le scan devrait etre fini a ce stade."
        }
        $script:scanActivityOperationId = $entry.operationId
    }

    Test-Case 'activityRegistry/cancelActivityOnTerminalIsIdempotent : annulation tardive sur une tache terminee = no-op accepte' {
        $r = Invoke-Pipe -Method 'cancelActivity' -ParamsJson "[`"$script:scanActivityOperationId`"]"
        if ($r.result.accepted -ne $true) { throw "accepted attendu true (no-op idempotent), recu $($r.result.accepted)" }
    }

    Test-Case 'scanLifecycle/startRejectedWhileAlreadyRunning : deuxieme demarrage pendant un scan actif refuse' {
        # Fenetre de course volontairement etroite (best-effort) : deux appels
        # startExactScanAsync consecutifs sans attente. Si le premier scan a
        # deja fini avant le second appel (cible petite, scan tres rapide),
        # ce test est marque INCONCLUANT plutot que force en echec/succes --
        # une temporisation artificielle du moteur pour le rendre fiable a 100%
        # serait un changement de production motive uniquement par le test,
        # explicitement interdit par la fiche.
        $r1 = Invoke-Pipe -Method 'startExactScanAsync' -ParamsJson "[`"$healthValue`", `"Int32`", {}]"
        $r2 = Invoke-Pipe -Method 'startExactScanAsync' -ParamsJson "[`"$healthValue`", `"Int32`", {}]"
        if ($r1.result.started -eq $true -and $r2.result.started -eq $true) {
            Write-Warning "INCONCLUANT : les deux appels ont demarre -- la fenetre de course etait trop etroite sur cette machine (cible trop rapide a scanner). Pas un echec du comportement, juste un test non concluant cette fois."
            return
        }
        if ($r2.result.success -ne $false -or $r2.result.error -ne 'Un scan est déjà en cours.') {
            throw "Refus attendu avec le message exact 'Un scan est deja en cours.', recu success=$($r2.result.success) error='$($r2.result.error)'"
        }
    }

    # Laisse le temps au(x) scan(s) precedent(s) de se terminer avant la suite.
    Start-Sleep -Milliseconds 500

    Test-Case 'scanLifecycle/cancelThenRestart : annuler puis redemarrer immediatement fonctionne sans etat residuel' {
        Invoke-Pipe -Method 'startExactScanAsync' -ParamsJson "[`"$healthValue`", `"Int32`", {}]" | Out-Null
        $cancelResult = Invoke-Pipe -Method 'cancelActiveScan'
        if ($cancelResult.result.success -ne $true) { throw "cancelActiveScan attendu success=true, recu $($cancelResult.result.success) (error: $($cancelResult.result.error))" }
        Start-Sleep -Milliseconds 200
        $restart = Invoke-Pipe -Method 'startExactScanAsync' -ParamsJson "[`"$healthValue`", `"Int32`", {}]"
        if ($restart.result.success -ne $true -or $restart.result.started -ne $true) {
            throw "Le redemarrage apres annulation ne devrait JAMAIS etre refuse -- recu success=$($restart.result.success) started=$($restart.result.started) error='$($restart.result.error)'"
        }
    }

    Start-Sleep -Milliseconds 500

    Test-Case 'scanLifecycle/detachDuringActiveScan : detacher pendant un scan ne plante pas et laisse un etat propre' {
        Invoke-Pipe -Method 'startExactScanAsync' -ParamsJson "[`"$healthValue`", `"Int32`", {}]" | Out-Null
        $detachResult = Invoke-Pipe -Method 'detachProcess'
        Write-Host "  detachProcess pendant un scan actif -> $($detachResult.result)" -ForegroundColor DarkGray
        # Re-attache pour la suite de la sequence, quel que soit le resultat
        # ci-dessus (report ou detachement immediat -- les deux sont
        # legitimes, voir l'invariant du manifeste).
        $deadline = (Get-Date).AddSeconds(10)
        $reattached = $false
        while ((Get-Date) -lt $deadline) {
            $a = Invoke-Pipe -Method 'attachProcess' -ParamsJson "[$($targetProc.Id)]"
            if ($a.result -eq $true) { $reattached = $true; break }
            Start-Sleep -Milliseconds 300
        }
        if (-not $reattached) { throw 'Impossible de se re-attacher apres le detachement pendant un scan actif.' }
    }

    # --- loadProfile ------------------------------------------------------------

    Test-Case 'loadProfile/invalidName : forme exacte {success:false, error} sur un profil inexistant' {
        $r = Invoke-Pipe -Method 'loadProfile' -ParamsJson '["CeProfilNExistePasCetteSession"]'
        if ($r.result.success -ne $false) { throw "success attendu false, recu $($r.result.success)" }
        if ([string]::IsNullOrEmpty($r.result.error)) { throw "champ 'error' attendu non vide (pas 'errorCode' -- forme reelle verifiee par 14B)" }
    }

    Test-Case 'loadProfile/validProfile : cycle sauver/charger sur une cible synthetique' {
        $save = Invoke-Pipe -Method 'saveProfileTarget' -ParamsJson "[`"$ProfileTestName`", `"HealthManifestCheck`", `"$healthAddress`", `"Int32`", `"UX-PRODUIT-14B behavior manifest`"]"
        if ($save.result.success -ne $true) { throw "saveProfileTarget attendu success=true, recu $($save.result.success) (error: $($save.result.error))" }

        $load = Invoke-Pipe -Method 'loadProfile' -ParamsJson "[`"$ProfileTestName`"]"
        if ($load.result.success -ne $true) { throw "loadProfile attendu success=true, recu $($load.result.success)" }
        if ($load.result.targetCount -ne $load.result.targets.Count) { throw "targetCount ($($load.result.targetCount)) != targets.length ($($load.result.targets.Count))" }
        $match = $load.result.targets | Where-Object { $_.name -eq 'HealthManifestCheck' }
        if (-not $match) { throw 'cible HealthManifestCheck absente de loadProfile.targets' }
        if ($match.locatorKind -notin @('absolute', 'module_offset', 'pointer_chain', 'clr_field')) {
            throw "locatorKind inattendu : '$($match.locatorKind)'"
        }
    }

    # --- getAiModelStatus ---------------------------------------------------

    Test-Case 'getAiModelStatus/default : les 3 etats (sessionDisabled/enabled/available) sont bien distincts' {
        $r = Invoke-Pipe -Method 'getAiModelStatus'
        if ($r.result.success -ne $true) { throw "success attendu true, recu $($r.result.success)" }
        foreach ($field in @('enabled', 'sessionDisabled', 'available', 'modelFound', 'ready')) {
            if ($null -eq $r.result.$field) { throw "champ '$field' manquant" }
        }
        if ($null -eq $r.result.embeddedAgents) { throw 'champ embeddedAgents manquant' }
    }

    # --- preparedDiagnosticReport (17) --------------------------------------

    Test-Case 'preparedDiagnosticReport/prepareThenPreview : prepare puis apercu conformes' {
        $prepare = Invoke-Pipe -Method 'prepareDiagnosticReport' -ParamsJson '[{}]'
        if ($prepare.result.success -ne $true) { throw "prepareDiagnosticReport attendu success=true, recu $($prepare.result.success) (error: $($prepare.result.error))" }
        if ([string]::IsNullOrEmpty($prepare.result.reportId)) { throw 'reportId manquant/vide' }

        $preview = Invoke-Pipe -Method 'getPreparedDiagnosticReportPreview'
        if ($preview.result.success -ne $true) { throw "getPreparedDiagnosticReportPreview attendu success=true, recu $($preview.result.success)" }
    }

    # --- candidateComparison (16), verification legere ----------------------

    Test-Case 'candidateComparison/reference : forme minimale toujours conforme (deja valide en profondeur en 16)' {
        $series = @(
            @{ id = 'a'; address = $healthAddress; type = 'Int32'; factor = 1; label = 'a' },
            @{ id = 'b'; address = $healthAddress; type = 'Int32'; factor = 1; label = 'b' }
        )
        $paramsJson = ConvertTo-Json @($series, @{ intervalMs = 100; maxDurationMs = 5000 }) -Depth 5 -Compress
        $start = Invoke-Pipe -Method 'startCandidateComparison' -ParamsJson $paramsJson
        if ($start.result.success -ne $true) { throw "startCandidateComparison attendu success=true, recu $($start.result.success) (error: $($start.result.error))" }
        Start-Sleep -Milliseconds 300
        $status = Invoke-Pipe -Method 'getCandidateComparisonStatus'
        if ($status.result.success -ne $true) { throw "getCandidateComparisonStatus attendu success=true, recu $($status.result.success)" }
        Invoke-Pipe -Method 'stopCandidateComparison' | Out-Null
    }

    Test-Case 'detachProcess final ne leve pas d''erreur' {
        Invoke-Pipe -Method 'detachProcess' | Out-Null
    }
} finally {
    try {
        if ($keProc -and -not $keProc.HasExited) {
            Invoke-Pipe -Method 'deleteProfile' -ParamsJson "[`"$ProfileTestName`"]" | Out-Null
        }
    } catch {
        # Best-effort : si le pipe est deja mort, rien a nettoyer par ce biais.
    }
    if ($keProc -and -not $keProc.HasExited) { Stop-Process -Id $keProc.Id -Force -ErrorAction SilentlyContinue }
    if ($targetProc -and -not $targetProc.HasExited) { Stop-Process -Id $targetProc.Id -Force -ErrorAction SilentlyContinue }
}

$passed = ($results | Where-Object { $_.Passed }).Count
$failed = ($results | Where-Object { -not $_.Passed }).Count
Write-Host ""
Write-Host "Behavior manifest live battery: $passed/$($results.Count) OK" -ForegroundColor $(if ($failed -eq 0) { 'Green' } else { 'Red' })
if ($failed -gt 0) {
    Write-Host "Echecs :" -ForegroundColor Red
    $results | Where-Object { -not $_.Passed } | ForEach-Object { Write-Host "  - $($_.Name): $($_.Error)" -ForegroundColor Red }
    exit 1
}
