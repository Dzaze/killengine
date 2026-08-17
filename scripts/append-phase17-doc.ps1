$ErrorActionPreference = 'Stop'
$path = 'docs\PHASE_TRACKER.md'
$content = [System.IO.File]::ReadAllText($path, [System.Text.Encoding]::UTF8)

if ($content.Contains('PHASE 17 - Performance IA')) { Write-Host 'Already documented'; exit 0 }

$marker = "      - [x] Non-regression lancement : hotkeys sans filtre natif avant enregistrement, init IA en fallback deterministe sans charger llama au demarrage"
$addition = @"

    - [x] PHASE 17 - Performance IA & solutions adaptees
      - [x] V1 Serveur llama persistant : nouveau module ``ai/llama_server`` demarre ``llama-server.exe`` une seule fois (modele charge en RAM, ``cache_prompt`` actif), completions HTTP local bornees, redemarrage auto si crash et fallback ``llama-cli`` si le serveur echoue
      - [x] V2 Prompts cacheables : prefixe statique (systeme + regles + exemples + outils) place en tete, contexte/historique/requete dynamiques en queue pour maximiser le hit du cache de tokens du serveur persistant
      - [x] V3 Retry correctif : seconde tentative avec rappel du format JSON quand le modele repond hors schema (tool call ou intention) au lieu de chuter directement sur le fallback deterministe
      - [x] V4 Historique conversationnel : ``AIEngine::noteOutcome`` enregistre chaque tour {query, outil, outcome} (borne a 12) et le transmet au prompt ; le modele sait ce qui a echoue et propose une vraie alternative
      - [x] V5 Contexte IA enrichi : aiContext expose desormais ``unknownSnapshotActive``, ``freezeCount`` et ``valueType`` en plus du contexte existant
      - [x] V6 Fallback deterministe adaptatif : variation decrite (augmente/diminue/change/stable) pendant un scan actif => ``next_scan`` increased/decreased/changed ; snapshot unknown actif => ``unknown_compare`` ; peu de candidats + cible connue => ``prepare_write_checkpoint`` (safe, sans ecrire) ; signalement d'echec apres exact_scan => relance ``exact_scan_multi_type``
      - [x] V7 Dispatch prepare_write_checkpoint : ApplicationController prepare les suggestions d'ecriture (``suggestedWrites``) sans ecrire, workflowStatus ``awaiting_write_confirmation``
      - [x] V8 Build staging : ``scripts/build.ps1`` copie aussi ``llama-server.exe`` dans ``build/bin``
      - [x] Tests : ``tests/unit/test_ai_adaptive.cpp`` (10 tests) + correction des 8 tests ContextualFallback preexistants qui omettaient ``engine.init()`` ; suite complete 115/115 OK
"@

if (-not $content.Contains($marker)) { Write-Host 'Marker not found'; exit 1 }
$content = $content.Replace($marker, $marker + $addition.Replace("`r`n", "`n").Replace("`n", "`r`n"))
[System.IO.File]::WriteAllText($path, $content, (New-Object System.Text.UTF8Encoding($false)))
Write-Host 'PHASE_TRACKER updated'