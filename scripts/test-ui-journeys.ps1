<#
AM-4 (docs/PHASE_TRACKER.md, 16/09/2026) : parcours UI essentiels rejouables,
pilotes reellement via CDP (Chrome DevTools Protocol) contre la vraie UI Qt/
WebEngine de KillEngine.exe -- pas des mutations directes du store Pinia, pas
seulement une survie de process. Le pipe d'automatisation sert uniquement a
preparer/observer la fixture (attacher/detacher, verifier une adresse), jamais
a remplacer un clic ou une saisie reelle dans l'UI.

Couvre les quatre parcours demandes par AM-4, dans cet ordre :
  1. Attache/detache synchronisee moteur -> UI, declenchee par le pipe (AM-2)
  2. Scan exact -> changement independant -> next-scan "decreased" -> resultat visible
  3. Bascule FR/EN visible dans l'UI et dans le store
  4. Sauvegarde d'un profil (locator module+offset, pas une adresse absolue) ->
     redemarrage de la cible -> rechargement -> diagnostic de durabilite
  + un cas d'annulation de confirmation RiskGate (Find What Writes) verifiant
    l'absence de tout effet.

Isolation :
  - Lance sa propre instance isolee (paquet portable frais, sans etat de
    session dev -- reutilise scripts/package-windows.ps1, deja garanti "propre"
    par AM-1) dans un dossier temporaire dedie, jamais dans build/bin directement.
  - Refuse de demarrer si une instance de KillEngine.exe/KillEngineTestTarget.exe
    tourne deja (le pipe KillEngineAutomationPipe est un nom global fixe -- ne
    jamais risquer de piloter/arreter l'instance du propriétaire).
  - Nom de profil unique par execution ; ProfileStore est portable depuis
    PORT-2b (17/09/2026) -- le profil vit sous $fixtureRoot\data\profiles et
    part avec le reste de la fixture en fin de run, jamais aux cotes des
    profils reels du propriétaire (c'etait encore le cas avant PORT-2b, voir
    docs/PHASE_TRACKER.md AM-1/AM-4).

Usage :
  .\scripts\test-ui-journeys.ps1
  .\scripts\test-ui-journeys.ps1 -KeepArtifacts
  .\scripts\test-ui-journeys.ps1 -Port 9421
#>
param(
    [switch]$KeepArtifacts,
    [int]$Port = 9420,
    [int]$LaunchTimeoutSec = 25
)

$ErrorActionPreference = 'Stop'

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot 'lib\cdp-client.ps1')
. (Join-Path $PSScriptRoot 'lib\win32-memory.ps1')

$buildBin = Join-Path $repoRoot 'build\bin'
$devKeExe = Join-Path $buildBin 'KillEngine.exe'
$devTargetExe = Join-Path $buildBin 'KillEngineTestTarget.exe'
$pipeCallScript = Join-Path $repoRoot 'scripts\automation-pipe-call.ps1'

if (-not (Test-Path -LiteralPath $devKeExe -PathType Leaf)) {
    throw "KillEngine.exe introuvable ($devKeExe). Lancer .\scripts\build.ps1 d'abord."
}
if (-not (Test-Path -LiteralPath $devTargetExe -PathType Leaf)) {
    throw "KillEngineTestTarget.exe introuvable ($devTargetExe). Lancer .\scripts\build.ps1 d'abord."
}

# Piege d'isolation documente dans AM-4 : le pipe est un nom de serveur global
# fixe (KillEngineAutomationPipe) -- une deuxieme instance ne cree pas un
# deuxieme serveur, et on ne doit jamais toucher/arreter l'instance de
# quelqu'un d'autre. Etat propre requis, sinon on saute proprement (pas un echec).
$existing = @(Get-Process -Name 'KillEngine', 'KillEngineTestTarget' -ErrorAction SilentlyContinue)
if ($existing.Count -gt 0) {
    $names = ($existing | ForEach-Object { "$($_.ProcessName) (PID $($_.Id))" }) -join ', '
    Write-Warning "SKIPPED: instance(s) deja en cours -- $names. Ce harnais ne touche jamais un process preexistant. Ferme ces process puis relance."
    exit 0
}

$runId = [Guid]::NewGuid().ToString('N').Substring(0, 8)
$fixtureRoot = Join-Path $env:TEMP "killengine_ui_journeys_$runId"
$artifactsRoot = Join-Path $repoRoot "dist\ui-journey-artifacts\$runId"
New-Item -ItemType Directory -Force -Path $artifactsRoot | Out-Null

$profileName = "am4-harness-$runId"
# PORT-2b (docs/PORTABILITY_ROADMAP.md, 17/09/2026) : ProfileStore::profilesDir()
# (core/profiles/profile_store.cpp) est desormais portable -- "data/profiles"
# relatif a l'executable (killcore::PortablePaths), plus %LOCALAPPDATA%. Avant
# ce lot, un profil cree pendant un dry-run vivait sous
# [Environment]::GetFolderPath('LocalApplicationData')\KillEngine\Profiles, aux
# cotes de vrais profils du propriétaire (jamais touches par ce harnais) ; il
# vit maintenant sous l'instance isolee elle-meme ($fixtureRoot), supprimee en
# bloc a la fin du run comme le reste de la fixture.
$profilesDir = Join-Path $fixtureRoot 'data\profiles'
$profileFile = Join-Path $profilesDir "$profileName.keprofile"

function Invoke-Pipe {
    param([string]$Method, [string]$ParamsJson = '[]', [int]$TimeoutMs = 8000)
    $raw = & $pipeCallScript -Method $Method -ParamsJson $ParamsJson -TimeoutMs $TimeoutMs
    return $raw | ConvertFrom-Json
}

function Wait-PipeReady {
    param([int]$TimeoutSec = 20)
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        try {
            $r = Invoke-Pipe -Method 'getVersion' -TimeoutMs 2000
            if ($r.result) { return $true }
        } catch {}
        Start-Sleep -Milliseconds 400
    }
    return $false
}

function Get-TestTargetMarker {
    param([int]$ProcessId, [int]$TimeoutSec = 10)
    $markerFile = Join-Path $env:TEMP 'killengine_test_target_addresses.txt'
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        if (Test-Path -LiteralPath $markerFile) {
            $lines = Get-Content -LiteralPath $markerFile
            $markerPid = $null; $healthAddr = $null
            foreach ($line in $lines) {
                if ($line -match '^pid=(\d+)$') { $markerPid = [int]$Matches[1] }
                if ($line -match '^g_health=(0x[0-9a-fA-F]+)$') { $healthAddr = $Matches[1] }
            }
            if ($markerPid -eq $ProcessId -and $healthAddr) {
                return [pscustomobject]@{ HealthAddress = $healthAddr }
            }
        }
        Start-Sleep -Milliseconds 300
    }
    throw "Marqueur g_health introuvable/PID non concordant dans $markerFile apres ${TimeoutSec}s (PID attendu $ProcessId)."
}

function ConvertTo-AddressNumber {
    param([string]$Hex)
    return [Convert]::ToUInt64(($Hex -replace '^0x', ''), 16)
}

# ---------------------------------------------------------------------------
# Preparation de la fixture isolee : reutilise scripts/package-windows.ps1
# (deja garanti sans etat de session dev par AM-1) plutot que de lancer
# KillEngine.exe directement depuis build/bin, qui accumule de vraies donnees
# de developpement (reglages, profil WebEngine, logs -- voir AM-1).
# ---------------------------------------------------------------------------
Write-Host "==> Preparation de la fixture isolee (paquet portable leger, sans modele/CLR/driver)" -ForegroundColor Cyan
# package-windows.ps1 ne pose pas de code de sortie explicite sur son dernier
# appel (Compress-Archive/Write-Host ne touchent pas $LASTEXITCODE) -- ses
# propres throw internes (ErrorActionPreference=Stop) sont ce qui protege
# reellement ce script si le paquetage echoue ; on verifie juste ici que le
# resultat attendu existe bel et bien plutot que de lire un $LASTEXITCODE
# qui ne refleterait pas fidelement ce script.
& (Join-Path $repoRoot 'scripts\package-windows.ps1') -SkipBuild -ExcludeModel -SkipClrInspector -SkipKernelDriver | Out-Null

$packagedRoot = Join-Path $repoRoot 'dist\KillEngine-portable'
if (-not (Test-Path -LiteralPath (Join-Path $packagedRoot 'KillEngine.exe') -PathType Leaf)) {
    throw "package-windows.ps1 n'a pas produit $packagedRoot\KillEngine.exe -- impossible de preparer une fixture isolee."
}
New-Item -ItemType Directory -Force -Path $fixtureRoot | Out-Null
robocopy $packagedRoot $fixtureRoot /E /NFL /NDL /NJH /NJS /NC /NS | Out-Null
if ($LASTEXITCODE -ge 8) {
    throw "robocopy a echoue (code $LASTEXITCODE) en copiant le paquet isole vers $fixtureRoot."
}
Copy-Item -LiteralPath $devTargetExe -Destination $fixtureRoot -Force

$fixtureKe = Join-Path $fixtureRoot 'KillEngine.exe'
$fixtureTarget = Join-Path $fixtureRoot 'KillEngineTestTarget.exe'

$results = New-Object System.Collections.Generic.List[object]
function Test-Journey {
    param([string]$Name, [scriptblock]$Body)
    Write-Host "[RUN] $Name" -ForegroundColor Cyan
    try {
        & $Body
        Write-Host "[OK]  $Name" -ForegroundColor Green
        $results.Add([pscustomobject]@{ Name = $Name; Passed = $true; Error = '' })
    } catch {
        Write-Host "[FAIL] $Name -- $_" -ForegroundColor Red
        $results.Add([pscustomobject]@{ Name = $Name; Passed = $false; Error = $_.ToString() })
        if ($script:cdpSession) {
            try {
                $shot = Join-Path $artifactsRoot ("failure_{0}.png" -f ($Name -replace '[^a-zA-Z0-9]+', '_'))
                Save-CdpScreenshot -Session $script:cdpSession -Path $shot
                Write-Host "       capture: $shot" -ForegroundColor Yellow
            } catch {
                Write-Warning "       (capture d'ecran d'echec impossible: $_)"
            }
        }
    }
}

$keProc = $null
$targetProc = $null
$script:cdpSession = $null

try {
    $env:KILLENGINE_AUTOMATION_PIPE = '1'
    $env:QTWEBENGINE_REMOTE_DEBUGGING = "127.0.0.1:$Port"
    $targetProc = Start-Process -FilePath $fixtureTarget -PassThru
    $keProc = Start-Process -FilePath $fixtureKe -PassThru
    Remove-Item Env:\KILLENGINE_AUTOMATION_PIPE -ErrorAction SilentlyContinue
    Remove-Item Env:\QTWEBENGINE_REMOTE_DEBUGGING -ErrorAction SilentlyContinue

    Start-Sleep -Milliseconds 500
    if ($targetProc.HasExited) { throw "KillEngineTestTarget.exe a quitte immediatement (code $($targetProc.ExitCode))." }
    if ($keProc.HasExited) { throw "KillEngine.exe a quitte immediatement (code $($keProc.ExitCode))." }

    Write-Host "==> Attente disponibilite backend (pipe + CDP), delai borne ${LaunchTimeoutSec}s" -ForegroundColor Cyan
    if (-not (Wait-PipeReady -TimeoutSec $LaunchTimeoutSec)) {
        throw "Pipe d'automatisation indisponible apres ${LaunchTimeoutSec}s."
    }
    $script:cdpSession = Connect-KillEngineCdpPage -Port $Port -TimeoutSec $LaunchTimeoutSec
    Wait-CdpCondition -Session $script:cdpSession -Expression "document.querySelectorAll('.nav-item').length > 0" -TimeoutSec $LaunchTimeoutSec -Description "UI Vue montee (.nav-item present)" | Out-Null

    # Une fixture fraichement isolee (jamais lancee) affiche l'ecran d'accueil
    # de premier lancement -- il PARTAGE les classes .risk-backdrop/.risk-btn
    # avec le vrai dialogue RiskGate (constate en dry-run : le clic Annuler du
    # parcours 5 touchait "Guide complet" de l'accueil, pas le vrai dialogue,
    # car document.querySelector('.risk-backdrop') renvoie le premier du DOM).
    # Le distingue par sa classe propre .onboarding-modal, absente du vrai
    # dialogue RiskGate (.risk-modal).
    Invoke-CdpEval -Session $script:cdpSession -Expression @'
(() => {
  const btn = document.querySelector('.onboarding-modal .risk-btn.primary');
  if (btn) { btn.click(); return 'DISMISSED'; }
  return 'NOT_PRESENT';
})()
'@ -NoAwaitPromise | Out-Null
    # Le calcul d'estimation de warmup IA locale peut aussi ouvrir un ecran
    # bloquant au tout premier lancement (docs/PHASE_TRACKER.md, chantier
    # "AI warmup splash + calibration") -- sans consequence fonctionnelle sur
    # les clics diriges par selecteur (element.click() ignore l'empilement
    # visuel), mais mieux vaut le fermer pour un etat propre.
    Invoke-CdpEval -Session $script:cdpSession -Expression @'
(() => {
  const btn = document.querySelector('.warmup-continue-btn');
  if (btn) { btn.click(); return 'DISMISSED'; }
  return 'NOT_PRESENT';
})()
'@ -NoAwaitPromise | Out-Null

    $marker = Get-TestTargetMarker -ProcessId $targetProc.Id
    $healthAddress = $marker.HealthAddress
    $healthNumber = ConvertTo-AddressNumber -Hex $healthAddress
    # Suit l'adresse de g_health courante (change apres le redemarrage de la
    # cible au parcours Profil -- nouvelle base ASLR) pour que le parcours
    # d'annulation puisse verifier la memoire reelle, quelle que soit l'adresse.
    $script:currentHealthAddress = $healthAddress

    # Langue de depart deterministe pour le reste des parcours (defaut app = fr,
    # voir ui/src/stores/settings.ts) -- un vrai clic UI, pas une mutation de store.
    Invoke-CdpClickByText -Session $script:cdpSession -Selector '.lang-switch-group button' -Text 'EN'
    Wait-CdpCondition -Session $script:cdpSession -Expression "document.body.innerText.includes('Ready')" -TimeoutSec 5 -Description "UI passee en anglais" | Out-Null

    # -----------------------------------------------------------------------
    # Parcours 1 : attache/detache synchronisee moteur -> UI (AM-2), pilotee
    # PAR LE PIPE (jamais un clic UI) pour prouver que le signal atteint la Vue.
    # -----------------------------------------------------------------------
    Test-Journey 'Attache/detache synchronisee (pipe -> UI, AM-2)' {
        $attach = Invoke-Pipe -Method 'attachProcess' -ParamsJson "[$($targetProc.Id)]"
        if ($attach.result -ne $true) { throw "attachProcess a echoue: $($attach | ConvertTo-Json -Compress)" }

        Wait-CdpCondition -Session $script:cdpSession -Expression "document.body.innerText.includes('Attached') && document.body.innerText.includes('KillEngineTestTarget.exe')" -TimeoutSec 10 -Description "UI reflete l'attache faite par pipe, sans clic ni rechargement" | Out-Null

        $detach = Invoke-Pipe -Method 'detachProcess' -ParamsJson '[]'
        if ($detach.result -ne $true) { throw "detachProcess a echoue ou a ete differe de facon inattendue: $($detach | ConvertTo-Json -Compress)" }
        Wait-CdpCondition -Session $script:cdpSession -Expression "document.body.innerText.includes('Ready')" -TimeoutSec 10 -Description "UI revenue a Ready apres detache par pipe" | Out-Null

        # Re-attache : necessaire pour les parcours suivants, et re-verifie le
        # meme mecanisme une deuxieme fois dans la foulee.
        $reattach = Invoke-Pipe -Method 'attachProcess' -ParamsJson "[$($targetProc.Id)]"
        if ($reattach.result -ne $true) { throw "Re-attache finale a echoue: $($reattach | ConvertTo-Json -Compress)" }
        Wait-CdpCondition -Session $script:cdpSession -Expression "document.body.innerText.includes('Attached') && document.body.innerText.includes('KillEngineTestTarget.exe')" -TimeoutSec 10 -Description "UI reflete la re-attache" | Out-Null
    }

    # -----------------------------------------------------------------------
    # Parcours 2 : scan exact -> changement independant (hors KillEngine, via
    # WriteProcessMemory direct) -> next-scan "decreased" -> resultat visible.
    # -----------------------------------------------------------------------
    Test-Journey 'Scan exact -> changement independant -> next-scan decreased' {
        Invoke-CdpClickByText -Session $script:cdpSession -Selector '[data-view="expert"]' -Text ''
        Wait-CdpCondition -Session $script:cdpSession -Expression "!!document.querySelector('.exact-controls input.input')" -TimeoutSec 10 -Description "panneau Exact Scan visible" | Out-Null

        # Type explicite (Int32) plutot que "Auto" : g_health est un int32 statique,
        # mais "Auto" teste plusieurs types a la fois. Un vrai process Qt/WebEngine
        # a des milliers de correspondances fortuites pour "100" (6934 constatees
        # en dry-run) -- bien plus qu'une seule page de candidats affiches, donc
        # g_health n'est PAS attendu visible tout de suite : la reduction se fait
        # par next-scan, pas par une premiere page qui contiendrait deja la bonne
        # adresse par chance.
        Invoke-CdpSetInputValue -Session $script:cdpSession -Selector '.exact-controls select.input.select' -Text 'Int32'
        Invoke-CdpSetInputValue -Session $script:cdpSession -Selector '.exact-controls input.input' -Text '100'
        Invoke-CdpClick -Session $script:cdpSession -Selector '.exact-controls button.btn-primary'
        # Attente fixe courte avant de sonder le spinner : le scan est rapide
        # (~100-300ms sur cette fixture) et peut demarrer/finir entre deux appels
        # CDP -- sonder ".btn-spinner absent" tout de suite apres le clic peut
        # lire l'etat "pas encore commence" (spinner jamais vu) et croire, a
        # tort, que le scan est deja termine (constate en dry-run).
        Start-Sleep -Milliseconds 400
        Wait-CdpCondition -Session $script:cdpSession -Expression "document.querySelector('.exact-controls button.btn-primary') && !document.querySelector('.exact-controls .btn-spinner')" -TimeoutSec 15 -Description "scan exact termine (spinner disparu)" | Out-Null

        # @(...) est indispensable : PowerShell aplatit un tableau JS a 1 seul
        # element en scalaire au retour de fonction, ce qui ferait ensuite
        # indexer un CARACTERE de la chaine ("0") au lieu de l'adresse entiere
        # -- deja constate en dry-run (comparaison numerique tombant sur 0).
        $initialCandidates = @(Invoke-CdpEval -Session $script:cdpSession -Expression "[...document.querySelectorAll('.candidate-row .address-btn')].map(b => b.textContent.trim())" -NoAwaitPromise)
        if (-not $initialCandidates -or $initialCandidates.Count -eq 0) { throw "Aucun candidat retourne par le scan exact (valeur 100)." }

        # Reduction par next-scan "exact" (valeur precise), pas "decreased" (trop
        # large sur un process reel ou beaucoup de compteurs/timers diminuent
        # aussi en permanence) -- change g_health en dehors de KillEngine (simule
        # un vrai jeu, voir scripts/lib/win32-memory.ps1). Verifie en direct
        # (dry-run) : une seule ronde exact 100->90 fait passer le pool de 7264
        # a 1 candidat, exactement g_health.
        Write-Int32ToExternalProcess -ProcessId $targetProc.Id -AddressHex $healthAddress -Value 90
        $verifyWrite = Invoke-Pipe -Method 'readMemoryPreview' -ParamsJson "[`"$healthAddress`", 4]"
        if ($verifyWrite.result.success -ne $true -or $verifyWrite.result.hex -ne '5A 00 00 00') {
            throw "L'ecriture externe vers g_health n'a pas ete confirmee avant le next-scan (attendu 5A 00 00 00 / 90, lu: $($verifyWrite.result.hex))."
        }

        Invoke-CdpSetInputValue -Session $script:cdpSession -Selector '.next-controls select.input.select' -Text 'exact'
        Invoke-CdpSetInputValue -Session $script:cdpSession -Selector '.next-controls input.input' -Text '90'
        Invoke-CdpClick -Session $script:cdpSession -Selector '.next-controls button.btn-primary'
        Start-Sleep -Milliseconds 400
        Wait-CdpCondition -Session $script:cdpSession -Expression "document.querySelector('.next-controls button.btn-primary') && !document.querySelector('.next-controls .btn-spinner')" -TimeoutSec 15 -Description "next-scan exact (90) termine (spinner disparu)" | Out-Null

        $reduced = @(Invoke-CdpEval -Session $script:cdpSession -Expression "[...document.querySelectorAll('.candidate-row .address-btn')].map(b => b.textContent.trim())" -NoAwaitPromise)
        if (-not $reduced -or $reduced.Count -eq 0) { throw "Aucun candidat restant apres le next-scan 'exact' (90)." }
        $selectorIndex = -1
        for ($i = 0; $i -lt $reduced.Count; $i++) {
            if ((ConvertTo-AddressNumber -Hex $reduced[$i]) -eq $healthNumber) { $selectorIndex = $i; break }
        }
        if ($selectorIndex -lt 0) { throw "g_health (0x$($healthAddress -replace '^0x','')) absent des $($reduced.Count) candidat(s) apres le next-scan 'exact' (90) -- candidats: $($reduced -join ', ')." }
        if ($reduced.Count -ge $initialCandidates.Count) { throw "Le next-scan 'exact' n'a pas reduit le nombre de candidats ($($initialCandidates.Count) -> $($reduced.Count))." }

        # Selectionne g_health pour le parcours Profil suivant (vrai clic sur sa ligne).
        Invoke-CdpClick -Session $script:cdpSession -Selector ".candidate-row:nth-of-type($($selectorIndex + 1)) .address-btn"
        Wait-CdpCondition -Session $script:cdpSession -Expression "true" -TimeoutSec 1 -Description "selection appliquee" | Out-Null
    }

    # -----------------------------------------------------------------------
    # Parcours 3 : bascule FR/EN visible dans l'UI ET dans le store, puis
    # retour a l'anglais (etat attendu par le reste du run).
    # -----------------------------------------------------------------------
    Test-Journey 'Bascule FR/EN visible (UI + store)' {
        Invoke-CdpClickByText -Session $script:cdpSession -Selector '.lang-switch-group button' -Text 'FR'
        $isFr = Wait-CdpCondition -Session $script:cdpSession -Expression 'document.querySelector("#app").__vue_app__.config.globalProperties["$pinia"]._s.get("settings").appLanguage === "fr"' -TimeoutSec 5 -Description "store.appLanguage === 'fr'"
        if (-not $isFr) { throw "Le store n'est pas passe en 'fr' apres le clic FR." }
        $frText = Get-CdpText -Session $script:cdpSession -Selector 'body'
        if ($frText -notmatch 'Attaché') { throw "Aucun texte francais visible (ex: 'Attaché') apres bascule FR." }

        Invoke-CdpClickByText -Session $script:cdpSession -Selector '.lang-switch-group button' -Text 'EN'
        $isEn = Wait-CdpCondition -Session $script:cdpSession -Expression 'document.querySelector("#app").__vue_app__.config.globalProperties["$pinia"]._s.get("settings").appLanguage === "en"' -TimeoutSec 5 -Description "store.appLanguage === 'en'"
        if (-not $isEn) { throw "Le store n'est pas revenu en 'en' apres le clic EN." }
        $enText = Get-CdpText -Session $script:cdpSession -Selector 'body'
        if ($enText -notmatch 'Attached') { throw "Aucun texte anglais visible (ex: 'Attached') apres retour EN." }
    }

    # -----------------------------------------------------------------------
    # Parcours 4 : sauvegarde d'un profil (locator module+offset reutilisable,
    # pas une adresse absolue) -> redemarrage de la cible -> rechargement ->
    # diagnostic de durabilite.
    # -----------------------------------------------------------------------
    Test-Journey 'Profil : sauvegarde -> redemarrage cible -> diagnostic durabilite' {
        Invoke-CdpClickByText -Session $script:cdpSession -Selector '[data-view="profiles"]' -Text ''
        Wait-CdpCondition -Session $script:cdpSession -Expression "!!document.querySelector('input[placeholder]')" -TimeoutSec 10 -Description "vue Profils montee" | Out-Null

        # Cree le profil (nom unique a ce run) -- ui/src/views/ProfileView.vue:1174-1187.
        Invoke-CdpSetInputValue -Session $script:cdpSession -Selector '.create-row input.scan-input' -Text $profileName
        Invoke-CdpClick -Session $script:cdpSession -Selector '.create-row button.btn-secondary'
        Wait-CdpCondition -Session $script:cdpSession -Expression "!!document.querySelector('.save-target-box')" -TimeoutSec 5 -Description "bloc de sauvegarde de cible visible (profil cree)" | Out-Null

        # Renseigne le nom de cible et sauvegarde (candidat g_health selectionne au parcours 2).
        Invoke-CdpSetInputValue -Session $script:cdpSession -Selector '.save-target-box .scan-input:nth-of-type(1)' -Text 'g_health'
        Invoke-CdpClick -Session $script:cdpSession -Selector '.save-target-box button.btn-primary'
        $statusOk = Wait-CdpCondition -Session $script:cdpSession -Expression "document.querySelector('.status-message')?.textContent ?? ''" -TimeoutSec 10 -Description "message de statut apres sauvegarde"
        if ($statusOk -notmatch '✓') { throw "La sauvegarde de la cible de profil n'a pas confirme de succes (statut: $statusOk)." }

        $locatorText = Get-CdpText -Session $script:cdpSession -Selector '.target-locator'
        if (-not $locatorText) { throw "Aucun locator affiche apres sauvegarde de la cible." }
        if ($locatorText -match '^0x[0-9a-fA-F]+$') { throw "Le locator sauvegarde est une adresse absolue brute ($locatorText), pas un locator module+offset reutilisable." }

        if (-not (Test-Path -LiteralPath $profileFile -PathType Leaf)) {
            throw "Fichier profil attendu introuvable: $profileFile"
        }

        # Redemarre la cible (nouvelle instance, nouveau PID, nouvelle base ASLR)
        # -- c'est exactement ce qu'un locator module+offset doit survivre.
        Stop-Process -Id $targetProc.Id -Force -ErrorAction SilentlyContinue
        Start-Sleep -Milliseconds 500
        $script:targetProc = Start-Process -FilePath $fixtureTarget -PassThru
        Start-Sleep -Milliseconds 800
        if ($script:targetProc.HasExited) { throw "KillEngineTestTarget.exe (redemarre) a quitte immediatement." }
        $newMarker = Get-TestTargetMarker -ProcessId $script:targetProc.Id
        if ((ConvertTo-AddressNumber -Hex $newMarker.HealthAddress) -eq $healthNumber) {
            Write-Warning "Adresse g_health identique avant/apres redemarrage (ASLR peut etre desactive sur cette machine) -- le test de durabilite reste valide mais moins discriminant."
        }
        $script:currentHealthAddress = $newMarker.HealthAddress

        $reattach = Invoke-Pipe -Method 'attachProcess' -ParamsJson "[$($script:targetProc.Id)]"
        if ($reattach.result -ne $true) { throw "Re-attache a la cible redemarree a echoue." }
        Wait-CdpCondition -Session $script:cdpSession -Expression "document.body.innerText.includes('Attached')" -TimeoutSec 10 -Description "UI re-attachee a la cible redemarree" | Out-Null

        # Diagnostic de durabilite (bouton reel "Check saved profile entries").
        Invoke-CdpClickByText -Session $script:cdpSession -Selector 'button' -Text 'Check saved profile entries'
        $entryStatusText = Wait-CdpCondition -Session $script:cdpSession -Expression "document.querySelector('.durability-entry summary')?.textContent ?? ''" -TimeoutSec 10 -Description "entree de durabilite affichee"
        if ($entryStatusText -notmatch 'g_health') { throw "L'entree de durabilite ne mentionne pas g_health (texte: $entryStatusText)." }
        if ($entryStatusText -match 'missing|version changed|wrong process') {
            throw "Diagnostic de durabilite en echec apres redemarrage de la cible (texte: $entryStatusText) -- le locator module+offset ne s'est pas re-resolu."
        }
    }

    # -----------------------------------------------------------------------
    # Cas d'annulation de confirmation : RiskGate sur une ecriture memoire
    # (WritePanel), verifie l'ABSENCE REELLE de modification via une relecture
    # memoire independante par le pipe (pas juste que le dialogue s'est ferme).
    # -----------------------------------------------------------------------
    Test-Journey 'Annulation RiskGate (ecriture memoire) sans effet' {
        Invoke-CdpClickByText -Session $script:cdpSession -Selector '[data-view="expert"]' -Text ''
        Wait-CdpCondition -Session $script:cdpSession -Expression "!!document.querySelector('.write-controls')" -TimeoutSec 10 -Description "panneau Ecrire visible" | Out-Null

        $before = Invoke-Pipe -Method 'readMemoryPreview' -ParamsJson "[`"$script:currentHealthAddress`", 4]"
        if ($before.result.success -ne $true) { throw "Lecture memoire prealable de g_health a echoue: $($before | ConvertTo-Json -Compress)" }
        $beforeHex = $before.result.hex

        # Adresse saisie manuellement (le panneau Ecrire accepte une adresse
        # libre quand aucun candidat n'est selectionne) + valeur deliberement
        # differente -- seul le clic Annuler doit empecher l'ecriture.
        Invoke-CdpSetInputValue -Session $script:cdpSession -Selector '.write-controls input.input:nth-of-type(1)' -Text ($script:currentHealthAddress -replace '^0x', '')
        Invoke-CdpSetInputValue -Session $script:cdpSession -Selector '.write-controls input.input:nth-of-type(2)' -Text '424242'
        # La carte memoire n'a jamais ete chargee dans cette session : l'adresse
        # est alors "region inconnue" du point de vue du garde-fou local (pas
        # un vrai probleme d'acces), ce qui affiche une case a cocher de
        # confirmation avant d'activer le bouton Ecrire -- WritePanel.vue:174-180.
        Invoke-CdpEval -Session $script:cdpSession -Expression @'
(() => {
  const cb = document.querySelector('.safety-ack input[type=checkbox]');
  if (cb && !cb.checked) cb.click();
  return 'OK';
})()
'@ -NoAwaitPromise | Out-Null
        Invoke-CdpClick -Session $script:cdpSession -Selector '.write-controls button.btn-primary'
        # Scope precis sur .risk-modal (dialogue RiskGate reel) : l'ecran
        # d'accueil de premier lancement partage .risk-backdrop/.risk-btn mais
        # utilise .onboarding-modal, deja ecarte au demarrage du harnais.
        Wait-CdpCondition -Session $script:cdpSession -Expression "!!document.querySelector('.risk-modal')" -TimeoutSec 5 -Description "dialogue RiskGate ouvert (ecriture)" | Out-Null

        Invoke-CdpClick -Session $script:cdpSession -Selector '.risk-modal .risk-btn.secondary'
        Wait-CdpCondition -Session $script:cdpSession -Expression "!document.querySelector('.risk-modal')" -TimeoutSec 5 -Description "dialogue RiskGate ferme apres Annuler" | Out-Null

        $after = Invoke-Pipe -Method 'readMemoryPreview' -ParamsJson "[`"$script:currentHealthAddress`", 4]"
        if ($after.result.success -ne $true) { throw "Lecture memoire apres annulation a echoue: $($after | ConvertTo-Json -Compress)" }
        if ($after.result.hex -ne $beforeHex) {
            throw "La memoire a change malgre l'annulation du dialogue RiskGate (avant: $beforeHex, apres: $($after.result.hex))."
        }
    }
} finally {
    if ($keProc -and -not $keProc.HasExited) { Stop-Process -Id $keProc.Id -Force -ErrorAction SilentlyContinue }
    if ($script:targetProc -and -not $script:targetProc.HasExited) { Stop-Process -Id $script:targetProc.Id -Force -ErrorAction SilentlyContinue }
    elseif ($targetProc -and -not $targetProc.HasExited) { Stop-Process -Id $targetProc.Id -Force -ErrorAction SilentlyContinue }
    if ($script:cdpSession) { Close-CdpSession -Session $script:cdpSession }

    # QtWebEngine demarre des process enfants (renderer/GPU/network) qui ne
    # meurent pas forcement de facon synchrone avec Stop-Process sur le
    # process parent -- sans cette attente, Remove-Item peut trouver certains
    # fichiers du dossier fixture encore verrouilles et n'en supprimer qu'une
    # partie en silence (-ErrorAction SilentlyContinue), constate en dry-run
    # (quelques DLL Qt residuelles sous imageformats\ apres un run pourtant vert).
    Start-Sleep -Milliseconds 800

    if (Test-Path -LiteralPath $profileFile -PathType Leaf) {
        Remove-Item -LiteralPath $profileFile -Force -ErrorAction SilentlyContinue
    }
    if (-not $KeepArtifacts) {
        Remove-Item -LiteralPath $fixtureRoot -Recurse -Force -ErrorAction SilentlyContinue
        if (Test-Path -LiteralPath $fixtureRoot) {
            Start-Sleep -Milliseconds 800
            Remove-Item -LiteralPath $fixtureRoot -Recurse -Force -ErrorAction SilentlyContinue
        }
        if ((Get-ChildItem -LiteralPath $artifactsRoot -ErrorAction SilentlyContinue | Measure-Object).Count -eq 0) {
            Remove-Item -LiteralPath $artifactsRoot -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
}

$passed = ($results | Where-Object { $_.Passed }).Count
$failed = ($results | Where-Object { -not $_.Passed }).Count
Write-Host ""
Write-Host "Parcours UI : $passed/$($results.Count) OK" -ForegroundColor $(if ($failed -eq 0) { 'Green' } else { 'Red' })
if ($failed -gt 0) {
    Write-Host "Echecs :" -ForegroundColor Red
    $results | Where-Object { -not $_.Passed } | ForEach-Object { Write-Host "  - $($_.Name): $($_.Error)" -ForegroundColor Red }
    if (-not $KeepArtifacts) {
        Write-Host "(captures d'ecran d'echec supprimees -- relancer avec -KeepArtifacts pour les conserver)" -ForegroundColor Yellow
    } else {
        Write-Host "Captures d'ecran : $artifactsRoot" -ForegroundColor Yellow
    }
    exit 1
}
exit 0
