# Guide de contexte pour les assistants IA (Cline, Codex, Claude, etc.)

Ce fichier aide les IA à comprendre rapidement le projet KillEngine et à travailler efficacement.

## ⚠️ Équipe d'agents : 3 modèles IA travaillent en parallèle

KillEngine n'est **pas** développé par un seul assistant. Trois agents IA différents contribuent au même dépôt, souvent dans la même journée :

| Agent | Runtime | Terrain habituel |
| --- | --- | --- |
| **Codex** (OpenAI) | Codex CLI | Backend C++ / core, moteur de scan, patches PowerShell sur les gros fichiers |
| **Cline + z.ai (GLM)** | Extension VS Code | Frontend Vue/TS, itérations UI, scripts |
| **Claude** (Anthropic) | Claude Code (CLI / extension VS Code) | Revue transverse, docs, refactor UX, cohérence produit |

Tous les commits sont signés par le même auteur Git (`Dzaze`) : **l'historique ne permet pas de savoir quel agent a écrit quoi**. Il faut donc partir du principe qu'un autre agent a pu modifier un fichier depuis ta dernière lecture.

### Règles de coexistence (obligatoires)

1. **Relire avant d'écrire.** Ne jamais éditer à partir d'un contenu mémorisé d'une session précédente : relire le fichier juste avant la modification.
2. **Édition ciblée, pas de réécriture massive.** Sur les fichiers partagés — `apps/desktop/application_controller.cpp` (5000+ lignes), `ui/src/views/ExpertView.vue` (5200+ lignes), `ui/src/stores/app.ts`, `ui/src/services/backend.ts` — préférer des remplacements chirurgicaux. Une réécriture complète écrase silencieusement le travail d'un autre agent.
3. **Une branche par chantier** : `agent/<sujet>` (ex. `agent/sc2-ui-string-tracker`). Ne pas committer directement sur `main`.
4. **`docs/PHASE_TRACKER.md` est le point de rendez-vous.** C'est là que les agents se parlent : cocher ce qui est fait, et noter ce qui est *en cours* pour éviter que deux agents attaquent la même phase.
5. **Contrat C++ ↔ Vue à respecter des deux côtés.** Une méthode `Q_INVOKABLE` ajoutée sans son entrée dans `ui/src/services/backend.ts` (interface **et** mock) casse le build de l'autre agent.
6. **Build + tests avant de rendre la main** : `.\scripts\build.ps1` puis `.\build\bin\killengine_unit_tests.exe`. Laisser le dépôt dans un état compilable.
7. **Encodage : UTF-8 sans BOM, obligatoire, quel que soit l'outil utilisé pour écrire le fichier.** Ce n'est pas une préférence de style, c'est une règle bloquante : un fichier mal réencodé casse l'affichage pour tous les autres agents et pour l'utilisateur. Déjà arrivé trois fois : `ExpertView.vue` (199 séquences), `app.ts` (13 séquences), `application_controller.cpp` (17 séquences — dans les messages du chat Assistant renvoyés à l'utilisateur, ex. `"Scan chiffré : ..."`). Corrigées le 16/08/2026. La dernière touchait du C++ (pas de l'UI), donc l'hypothèse la plus probable est un script PowerShell de patch backend, pas un outil frontend.

   **Cause technique** : PowerShell (surtout Windows PowerShell 5.1, `powershell.exe`, par opposition à PowerShell 7 `pwsh.exe`) n'écrit **pas** de l'UTF-8 par défaut.
   - `Set-Content`, `Add-Content`, `Out-File`, et les redirections `>` / `>>` sans `-Encoding` explicite écrivent en codepage ANSI système (souvent cp1252) ou en UTF-16LE selon la cmdlet et la version.
   - `[System.IO.File]::WriteAllText($path, $text)` sans encodage explicite ajoute un BOM UTF-8 non désiré.
   - Le résultat classique — texte lu correctement en UTF-8 puis réécrit en cp1252 — produit le double-encodage `é` → `Ã©`, `—` → `â€”`, etc.

   **À faire, sans exception**, quel que soit le langage du script qui écrit le fichier :
   - PowerShell (`pwsh`) : `Set-Content -Path $f -Value $text -Encoding utf8NoBOM` — jamais `Set-Content`/`Out-File`/`>` sans `-Encoding`.
   - .NET direct (le seul cas où un BOM existant doit être préservé, ex. patch ciblé sur un fichier qui en a déjà un) : lire en octets bruts, détecter le BOM, décoder en UTF-8, puis réécrire avec `New-Object System.Text.UTF8Encoding($hasBom)` — c'est le patron déjà utilisé correctement dans `scripts/patch-p1-expertview.ps1`, `patch-p1-store.ps1`, `patch-p1-tests.ps1` : à copier tel quel pour tout nouveau script de patch.
   - Python : `open(path, encoding='utf-8')` explicite (jamais compter sur l'encodage par défaut de l'OS).
   - Node/TS : `fs.writeFileSync(path, text, 'utf8')` — le défaut Node est déjà sûr, mais le préciser quand même par cohérence.

   **Vérification avant de rendre la main**, sur tout le dépôt et pas seulement `ui/src` (testée et fonctionnelle — `-Path` + `-Recurse` combinés échouent sur Select-String, passer par `Get-ChildItem`) :
   ```powershell
   Get-ChildItem -Path ui,apps,core,ai,docs -Recurse -File -Include *.vue,*.ts,*.cpp,*.h,*.md,*.json |
     Select-String -Pattern 'Ã[\x80-\xBF]|â€' -Encoding utf8
   ```
   Toute correspondance = fichier corrompu à réparer avant commit. Exception connue et volontaire : l'entrée « PHASE 18 UX » de `docs/PHASE_TRACKER.md` cite littéralement `` `RÃ©gion active` `` comme exemple du bug — elle matche le motif exprès, ne pas la "corriger".

## Démarrage rapide

```
# Build C++ + frontend
.\scripts\build.ps1

# Build frontend uniquement
cd ui && npm run build

# Run tests unitaires
.\build\bin\killengine_unit_tests.exe

# Run tests avec filtre
.\build\bin\killengine_unit_tests.exe --gtest_filter=ValueVariants.*

# Configurer le projet (première fois)
.\scripts\configure.ps1
```

## Stack technique

- **Langage** : C++20 (MSVC 2022)
- **Build** : CMake + Ninja
- **UI** : Vue 3 + TypeScript + Vite (compilée en `ui/dist/`)
- **Framework UI** : Qt 6.8 (QWebChannel bridge C++ ↔ Vue)
- **IA** : llama.cpp embarqué via `llama-cli.exe` + agents sous `model/<nom_ia>/` + poids GGUF partagés sous `model/qwen/`
- **Tests** : Google Test
- **Compression** : LZ4 (snapshots)
- **JSON** : nlohmann/json
- **Plateforme** : Windows 64-bit uniquement

## Architecture

```
killengine/
├── core/           # killcore.lib — moteur déterministe (mémoire, scanner, candidats)
│   ├── scanner/        # ScanEngine, value_variants, candidate_confidence
│   ├── candidates/     # CandidateStore (file-backed, pagination, undo)
│   ├── memory/         # MemoryMap, MemoryReader, MemoryWriter
│   ├── snapshot/       # SnapshotStore + LZ4 codec (unknown initial value)
│   ├── pointer/        # PointerChain + PointerScanner (chaines de pointeurs multi-niveau)
│   ├── freeze/         # FreezeManager (écriture périodique)
│   ├── profiles/       # ProfileStore + Locator (module_offset, absolute, pointer_chain)
│   ├── process/        # ProcessEnumerator, ProcessHandle
│   └── logging/        # Logger (fichier rotatif)
├── ai/             # killai.lib — moteur IA (tool-calling, intent contract)
│   ├── ai_engine.cpp   # AIEngine (IA embarquée llama.cpp + état dégradé déterministe)
│   ├── intent_contract # Validation des intentions structurées
│   ├── tool_registry   # Registre des outils disponibles
│   └── llama_runtime   # Runtime llama-cli embarqué, initialisé paresseusement
├── apps/desktop/   # KillEngine.exe — application desktop
│   ├── application_controller.cpp  # ⚠️ ~10 000 lignes, pont C++ ↔ Vue
│   └── main.cpp       # EntryPoint Qt + QWebChannel
├── ui/             # Frontend Vue 3
│   └── src/
│       ├── services/backend.ts   # Interface TypeScript du backend C++
│       ├── stores/app.ts         # Pinia store (état global)
│       ├── components/expert/    # Panneaux extraits d'ExpertView (InfoDot, RiskBadge...)
│       └── views/                # Assistant, Investigation, Expert, Trainer,
│                                 # Process, Memory, Profile, Settings
├── tests/          # Tests Google Test
│   ├── unit/          # Tests unitaires
│   └── integration/   # Tests avec KillEngineTestTarget.exe
├── docs/           # Documentation
│   └── PHASE_TRACKER.md  # ⭐ Source of truth pour l'avancement
├── scripts/        # Scripts PowerShell (build, configure, package)
└── CMakeLists.txt  # Configuration CMake racine
```

## Patterns clés à respecter

### Communication C++ ↔ Vue
- Le backend expose des méthodes `Q_INVOKABLE` sur `ApplicationController`
- Le frontend appelle via `backend.ts` → `QWebChannel`
- **Toujours** ajouter les nouvelles méthodes C++ dans `backend.ts` (interface TypeScript)
- Les signaux Qt (`scanStarted`, `scanFinished`, etc.) sont exposés comme `QWebChannelSignal<T>`

### Ajouter un nouveau fichier source
1. Créer le `.h` et `.cpp` dans le bon dossier `core/`
2. Ajouter le `.cpp` au `core/CMakeLists.txt` (section correspondante)
3. Si c'est un nouveau module, documenter le rôle dans ce fichier

### Tests
- Tests unitaires : `tests/unit/` → ajouté dans `tests/CMakeLists.txt`
- Toujours utiliser Google Test (`TEST(SuiteName, TestName) { ... }`)
- Les tests doivent être indépendants du processus cible ( KillEngineTestTarget)
- Les tests d'intégration (`tests/integration/`) lancent `KillEngineTestTarget.exe`

### Unknown Initial Value
- Après une capture unknown, l'utilisateur choisit la comparaison via les boutons guidés (`ça augmente`, `ça diminue`, `ça change`, `stable`).
- Le type Unknown peut être `Auto` : la première comparaison teste plusieurs représentations (`Int16`, `UInt16`, `Int32`, `UInt32`, `Int64`, `Float32`, `Float64`) et stocke des candidats typés mélangés.
- `stable` / `unchanged` est autorisé même en première comparaison, mais il est potentiellement très bruyant ; les comparaisons unknown restent bornées côté moteur pour éviter les retours massifs et les blocages UI.
- Les comparaisons unknown doivent rester bornées côté moteur pour éviter les retours massifs et les blocages UI.

### Pointer Chains
- Les chaines de pointeurs sont représentées par `core/pointer/PointerChain` et peuvent être stockées dans les profils via `LocatorKind::PointerChain`.
- Convention de résolution : chaque offset suit un déréférencement (`read pointer`, puis `+ offset`), y compris le dernier offset.
- Les nouvelles méthodes C++ exposées à l'UI doivent aussi être ajoutées dans `ui/src/services/backend.ts`.
- `suggestStableLocatorForAddress` (voir section Auto-résolution IA ci-dessous) est un raccourci autour de `scanPointerChains` pensé pour un usage post-écriture : ne pas dupliquer sa logique de bornes par défaut ailleurs.

### Trace UI string / moteurs modernes
- Objectif : retrouver les ressources affichées quand les scans numériques classiques ne trouvent rien, en partant des strings UI (`"45"`, `"50"`, etc.) puis en remontant vers les sources numériques ou les pointeurs qui les alimentent.
- UI principale : `ui/src/views/ExpertView.vue`, section **Trace UI string**.
  - `Scanner texte` cherche la valeur affichée en ASCII/UTF-16 dans les régions filtrées.
  - `Filtrer strings` est le next scan des strings : il garde les mêmes slots si la valeur change, et peut suivre une string déplacée dans une petite fenêtre proche.
  - `Analyser sources` cherche des formes numériques autour des strings suivies : `Int32`, `Int32 x100`, `Int32 x65536`, etc.
  - `Tracker sources` garde les sources numériques qui suivent la nouvelle valeur affichée.
  - `Auto origine` lance l'analyse source, essaie plusieurs rayons (`1 Mo`, `4 Mo`, `16 Mo`), sélectionne automatiquement les sources trouvées, prépare le panneau Write, puis inspecte les backrefs.
  - `Backrefs` / `Origine` cherchent les pointeurs 64-bit qui pointent près des strings exactes ; ne pas matcher tout l'intervalle entre strings éloignées.
  - `Démarrer enquête` / `Arrêter enquête` capture des snapshots rapides autour des strings/sources sélectionnées, plus des empreintes globales de blocs writable. À l'arrêt, l'app relit les blocs modifiés et cherche les variantes numériques de la nouvelle valeur affichée (`globalValueHits`), puis ajoute automatiquement ces pistes aux sources numériques sélectionnées.
- Backend exposé dans `ApplicationController` :
  - `scanUiStrings`
  - `trackUiStringCandidates`
  - `analyzeUiStringSources`
  - `trackUiStringSources`
  - `inspectUiStringOrigins`
  - `startUiStringInvestigation`
  - `finishUiStringInvestigation`
  - `writeMemoryValuesWithVariants`
- Logique pure testée dans `core/scanner/display_value_tracker.*`.
  - Tests ciblés : `.\build\bin\killengine_unit_tests.exe --gtest_filter=UiStringTracker.*`
- Écriture : le panneau **Write / Freeze** peut recevoir une sélection mixte issue des sources UI. L'utilisateur entre la valeur affichée (`60`) ; KillEngine calcule automatiquement la valeur réellement écrite selon le variant (`Int32` -> `60`, `Int32 x100` -> `6000`, `Int32 x65536` -> `3932160`) et affiche un **Plan d'écriture** avant le clic.
- Sécurité cibles exigeantes : les résultats radar peuvent retourner des centaines de pistes (`globalValueHits`). Ne pas tout écrire d'un coup par défaut : l'UI sélectionne un petit paquet initial et propose filtres type/variant, sélection par lot, `Top 25`, `Tout cocher sûr`, `Tout décocher`. Tester les écritures par petits lots avant freeze.
- Watch live : `Watch lot`, `Watch cochés`, `Watch page` et `Watch sélection` permettent d'envoyer jusqu'à 200 adresses au live watcher pour voir les valeurs bouger sans cliquer adresse par adresse. Les sources numériques affichent aussi leur valeur live directement dans la ligne.
- Freeze cibles qui réécrivent vite : le panneau **Write / Freeze** expose l'intervalle de polling (`16`, `33`, `50`, `100`, `250`, `500` ms) et appelle `setFreezeInterval`. Pour une valeur réécrite très vite, commencer à `16 ms`.
- Télémétrie : les actions Trace UI string écrivent dans `%LOCALAPPDATA%\KillEngine\KillEngine\logs\scan_telemetry.jsonl`.
  - Événements : `ui_string_scan`, `ui_string_track`, `ui_string_sources_analyze`, `ui_string_sources_track`, `ui_string_origins_inspect`, `ui_string_investigation_start`, `ui_string_investigation_finish`, `ui_string_sources_write`.
  - Lecture rapide après un test :
    `Get-Content "$env:LOCALAPPDATA\KillEngine\KillEngine\logs\scan_telemetry.jsonl" -Tail 80`
- Interprétation des moteurs à UI découplée :
  - Beaucoup de strings UI peuvent être de simples copies d'affichage, pas la source gameplay.
  - Des adresses basses de type `0x590A... -> 0x289...` ressemblent souvent à des tables de pointeurs UI ; ne pas les écrire comme des ressources.
  - Si `Valeurs radar = 0` malgré des `Blocs modifiés > 0`, la valeur nouvelle n'est probablement pas représentée directement dans les pages modifiées observées ; retenter en renseignant bien la nouvelle valeur affichée avant `Arrêter et comparer`, ou passer ensuite à un vrai mode debugger/hardware breakpoint pour capturer l'instruction qui écrit la string.

### Find What Writes / Debugger expérimental
- Module : `core/debug/hardware_breakpoint.*`.
- Backend : `ApplicationController::findWhatWrites(addressHex, options)` expose un premier utilitaire synchrone borné (`size`, `timeoutMs`, `maxHits`) via `ui/src/services/backend.ts`.
- UI : section **Trace UI string**, bouton `Écrit par` sur une source numérique proche. L'utilisateur doit cliquer, puis faire varier la valeur dans le jeu pendant la fenêtre de capture.
- Contraintes Windows : `DebugActiveProcess` / `WaitForDebugEvent` doivent rester dans le même thread logique pour l'utilitaire bloquant. Avant d'en faire un vrai workflow async, créer un worker dédié qui attache, attend et détache dans le même thread.
- Prudence debugger : l'attachement peut échouer, être détecté, ou perturber l'application cible. Garder cette fonctionnalité en mode Expert/expérimental tant qu'elle n'est pas validée manuellement sur des cibles autorisées.
- Piège corrigé le 16/08/2026 : `applyBreakpointsToThread` appelait `SetThreadContext` sur des threads en cours d'exécution sans les suspendre — silencieusement peu fiable sur Windows (la doc Win32 exige un thread suspendu pour un `SetThreadContext` garanti). Toujours suspendre/reprendre autour de `GetThreadContext`/`SetThreadContext` quand ce n'est pas fait depuis un événement de debug (où le process est déjà globalement gelé).

### Freeze par hardware breakpoint ("invincible freeze")
- Module : `core/debug/breakpoint_freeze.*` (`BreakpointFreezeManager`), au-dessus de `HardwareBreakpointSession`.
- Objectif : contrairement au freeze par polling (`core/freeze/freeze_manager.*`, `FreezeMode::Polling`), qui clignote entre deux ticks sur une cible qui réécrit vite, ce mode pose un hardware breakpoint DR0-DR3 et corrige la valeur *dans le cycle du debug event*, avant que le jeu ne reprenne la main.
- Modes : `RewriteValue` (recommandé — réécrit après coup, mode le plus robuste), `BlockWrite` (retombe actuellement sur `RewriteValue`, pas encore de vraie annulation de contexte CPU — v1 volontairement simple), `Capture` (juste compter, debug).
- Backend : `ApplicationController::freezeWithBreakpoint(addressHex, valueType, value, options)` / `stopBreakpointFreeze()`. Jusqu'à 4 adresses simultanées via `FreezeEntry`/`FreezeMode::HardwareBreakpoint` dans `m_freeze` (`core/freeze/freeze_manager.*`), redémarrées via `restartBreakpointFreezeFromRegistry`.
- UI : bouton `Freeze BP` dans le panneau Write/Freeze d'Expert. Chaque nouvelle adresse figée en mode BP redémarre la session multi-adresse depuis le registre.
- **Preuve de fiabilité mesurée** (pas juste "ça compile") : `tests/integration/test_power_up_runtime.cpp::BreakpointFreezeHoldsUnderFastRewriteStress`. Fait tourner `KillEngineTestTarget.exe` avec un thread interne qui réécrit sa propre mémoire (pas via `WriteProcessMemory` externe — voir piège ci-dessous) à ~1000 Hz pendant 1s, échantillonne ~5000 fois par spin-wait. Résultat stable sur plusieurs runs : chaque écriture est interceptée (`totalHits` ≈ 1000), et la valeur figée est visible ~80% du temps (le reste étant la latence physique réelle du cycle `WaitForDebugEvent` → `ReadProcessMemory` → `WriteProcessMemory` → `ContinueDebugEvent`, pas un bug). Seuil du test : 60%, avec marge sous la baseline mesurée.
- **Piège découvert en écrivant ce test, à connaître avant de retoucher ce module** : un hardware breakpoint DR0-DR3 ne se déclenche QUE pour une instruction exécutée sur un thread du processus cible qui porte ces registres. Un `WriteProcessMemory` externe (ce que fait KillEngine lui-même pour écrire une valeur, et ce qu'un premier jet de ce test utilisait par erreur) copie les octets en mode noyau sans jamais exécuter d'instruction sur un thread du debuggee : **le breakpoint ne se déclenche jamais** dans ce cas. C'est documenté et testé explicitement dans `BreakpointFreezeDoesNotInterceptExternalWriteProcessMemory`. Ne pas re-introduire un test qui simule "le jeu réécrit" via un writer externe — utiliser le rewriter interne de `KillEngineTestTarget.exe` (activé par la variable d'environnement `KILLENGINE_TEST_TARGET_STRESS_REWRITE`, adresse exacte de `g_health` exposée dans `%TEMP%\killengine_test_target_addresses.txt` pour éviter un scan par valeur, qui peut tomber sur n'importe quel autre Int32 du process Qt qui vaut coïncidemment la même chose).
- Tests ciblés : `.\build\bin\killengine_integration_tests.exe --gtest_filter=PowerUpRuntimeTest.BreakpointFreeze*` (nécessite les privilèges debug ; `GTEST_SKIP` sinon).
- `ApplicationController::getBreakpointFreezeStats()` (17/08/2026) expose en direct les stats déjà collectées par `BreakpointFreezeManager::stats()` (hits/rewrites/errors/healthy), plutôt que d'attendre `stopBreakpointFreeze()`. UI : badge `bp-live-stats` dans le panneau Write d'Expert, sondé toutes les 1s pendant que `store.breakpointFreezeEnabled` est vrai (`ExpertView.vue`).

### Détection automatique d'instabilité freeze (17/08/2026) — "le freeze qui clignote" sans que l'utilisateur le signale
- Objectif produit : un utilisateur premium ne devrait jamais avoir à deviner que son freeze ne tient pas — KillEngine doit le remarquer et le dire.
- Module : `core/freeze/freeze_manager.h/.cpp`. `FreezeEntry` porte maintenant `consecutiveDriftTicks`/`totalDriftTicks`/`totalTicks`/`flaggedUnstable`. `FreezeManager::recordPollTick(address, valueMatchedBeforeRewrite)` incrémente ces compteurs et retourne `true` la première fois qu'une entrée franchit `kFreezePollDriftThreshold` (5 ticks consécutifs de dérive) — ne notifie qu'une seule fois par activation, `setEntry()` réinitialise l'état de fiabilité si l'adresse est réarmée.
- `ApplicationController::applyFreezeTick()` (le tick du freeze polling, `apps/desktop/application_controller.cpp`) lit maintenant la valeur **avant** de la réécrire, compare à la cible, appelle `recordPollTick`, puis réécrit dans tous les cas. Au franchissement du seuil : `emit freezeInstabilityDetected(info)` avec adresse, type, compteurs, taux de tenue et un message + une suggestion en français prêts à afficher.
- Frontend : `ui/src/stores/app.ts` connecte ce signal dans `init()` et pousse un message Assistant automatiquement (dédupliqué par adresse). Aucune escalade automatique vers Freeze BP ou Find What Writes — les deux attachent un debugger, donc restent des actions à confirmer explicitement, la détection ne fait que le signaler.
- **Portée volontairement limitée au mode polling.** Le mode Freeze BP est déjà mesuré fiable (~80% à 1kHz, voir plus haut) et son architecture est event-driven (pas de tick régulier sur le thread principal), donc le même mécanisme de dérive ne s'y applique pas tel quel — `getBreakpointFreezeStats()` (ci-dessus) couvre la visibilité sans construire un deuxième minuteur dédié pour l'instant.
- Tests : `tests/unit/test_power_up_modules.cpp`, suite `FreezeManager` — tient (pas de notification), franchit le seuil une seule fois, récupère (le compteur consécutif retombe mais l'historique reste), réarmement réinitialise l'état.

### Injection DLL / Hooking in-process
- Module : `core/inject/*` (`dll_injector.*`, `function_hook.*`) — `CreateRemoteThread` + `LoadLibraryW`, génération de shellcode `jmp` court/long, calcul de taille de trampoline.
- Statut : utilitaires bas niveau testés (`tests/integration/test_power_up_runtime.cpp` : `InjectDllFailsCleanlyOnMissingDll`, `InlineHookHelpersProduceValidShellcode`, `InjectShellcodeRetDoesNotCrashTarget`), mais **pas encore branchés à un workflow Trainer/Expert visible côté UI**. C'est un module Phase 20 (`docs/POWER_UP_ROADMAP.md` section B) en construction : ne pas supposer qu'il est exposé à l'utilisateur avant de vérifier `ui/src/views/ExpertView.vue` et `ui/src/services/backend.ts`.

### Auto-résolution IA
- `ai/auto_resolver.*` (`killai::AutoResolver`) : ne contient plus que ce qui sert réellement.
  - `planForGoal(goal)` génère le plan d'affichage montré à l'utilisateur — appelé depuis `startAutoResolve`.
  - `computeAutoResolveTelemetryReport(events)` est une fonction pure qui analyse les événements telemetry récents et produit les insights actionnables (`AutoResolveTelemetryInsight { id, label, reason, nextAction, safe }`) + le rapport "valeur affichée découplée de la source mémoire". C'est la **seule** implémentation de cette logique — `ApplicationController::getAutoResolveReport` l'appelle directement au lieu de la dupliquer en ligne. Testée dans `tests/unit/test_auto_resolver.cpp` (y compris le seuil `kTraceUiSourceOverflowThreshold` pour le cas "500 candidats après Analyser sources").
  - **Retiré le 16/08/2026** : `executePlan`/`executeStep`/`resolve`/`validateStepResult` et `summarizeAutoTelemetry` — deux implémentations mortes, jamais appelées, qui dupliquaient (avec des noms d'outils incompatibles) la logique réelle du state-machine. Si un historique git plus ancien montre ces méthodes, elles ont été retirées volontairement, pas perdues par accident.
- Le moteur d'exécution du scan reste le state-machine écrit à la main dans `ApplicationController::startAutoResolve` (`apps/desktop/application_controller.cpp`, ~350 lignes) : scan exact multi-type → fallback scan chiffré XOR borné → fallback Trace UI string borné → capture Unknown bornée, avec `executedSafeSteps` audité et `requiresConfirmation` avant tout write/freeze.
- **Chaînage réel write → find-what-writes → AOB → patch (16/08/2026)** : dès qu'une capture Find What Writes réussit (`findWhatWritesForSource`/`findWhatWritesForUiString` dans `ExpertView.vue`), `autoChainFindWhatWritesResult()` enchaîne automatiquement sur `generateAobSignatureFromHit()` (le même chemin que le clic manuel "Analyser", rien de dupliqué côté backend) : signature AOB + suggestions de patch apparaissent sans clic supplémentaire. Reste strictement lecture seule — sauvegarder en Trainer (`saveTrainerPatchFromHit`) et appliquer un patch (`applyCodePatch`) restent des actions manuelles explicites.
- `suggestStableLocatorForAddress(addressHex, options)` : après une écriture confirmée sur une adresse unique, cherche une chaîne de pointeurs stable pour qu'elle survive à un redémarrage. Lecture seule, bornée (depth=3/offset=0x1000/results=5 par défaut). Déclenchée automatiquement en fond après une écriture Expert réussie (`autoSuggestStableLocatorIfWorthwhile` dans `ui/src/stores/app.ts`, silencieux si rien trouvé, dédupliqué par adresse) — `scanPointerChains` reste l'opération la plus lente de l'app, donc jamais lancée en boucle.
- **Ce qui manque encore pour un chaînage complet** : rien ne déclenche automatiquement `findWhatWritesAsync` après une écriture confirmée qui ne tient pas (ex: freeze qui clignote). Ça demande soit une détection "la valeur est repartie" (pas encore instrumentée), soit une intention en langage naturel ("ça ne tient pas") côté `ai_engine.cpp`/`tool_registry.cpp` — aucune des deux n'existe aujourd'hui. La partie qui existe déjà et qui chaîne réellement commence à partir du clic "Écrit par".

### AOB signatures / Trainer engine
- Module : `core/patch/aob_scanner.*`.
- Objectif : passer du scan de valeurs RAM à une approche trainer type WeMod/Wand : retrouver une signature d'instruction stable dans les régions code, puis plus tard patcher/hooker cette instruction.
- Backend : `ApplicationController::scanAobPattern(pattern, options)` expose le scan de pattern (`48 8B ?? 89`) avec filtres `executableOnly`, `imageOnly`, `maxResults`; `generateAobSignature(address, options)` lit les bytes autour d'un RIP capturé pour produire une signature brute; `suggestCodePatches(address, options)` utilise Zydis v4.1.1 pour décoder l'instruction, générer une signature stable avec wildcards sur immediates/displacements, classer le type d'instruction et proposer des templates; `applyCodePatch` / `restoreCodePatch` écrivent des bytes exacts et restaurent les bytes originaux gardés en mémoire.
- Profils : `ProfileStore` persiste aussi `patches` dans les `.keprofile` (nom, module, offset, AOB stable, bytes patchés, bytes originaux, désassemblage, risque). `saveProfileCodePatch`, `applyProfileCodePatch`, `restoreProfileCodePatch`, `applyAllProfileCodePatches`, `restoreAllProfileCodePatches`, `inspectProfileCodePatches` permettent de construire, piloter et vérifier un mini trainer réutilisable.
- UI : section **AOB signatures** dans Expert. Les hits `Écrit par` peuvent remplir automatiquement le pattern AOB via le bouton `Signature`; les matches AOB peuvent être analysés pour afficher le désassemblage, remplir une AOB stable, proposer des patchs classés (`low`/`medium`/`high`) comme `NOP écriture`, `Forcer non pris`, `Forcer pris`, `INT3 debug`, `RET + NOP`, puis patcher/restaurer ou sauvegarder dans un profil trainer.
- Tests ciblés : `.\build\bin\killengine_unit_tests.exe --gtest_filter=AobScanner.*`.
- Prochaine étape logique : ajouter une vue trainer dédiée avec toggles persistants et warnings anti-multi-match bloquants avant application.

### Performance adaptive
- Préférer plus de threads contrôlés à plus de processus : le moteur doit rester déterministe et l'UI Qt/Vue doit rester fluide.
- Centraliser les décisions machine dans `core/scanner/performance_profile.*` plutôt que disperser des heuristiques dans `ScanEngine` ou `ApplicationController`.
- Le mode `Auto` doit garder au moins un thread logique disponible pour Windows/l'UI, plafonner la mémoire en vol, et rester borné sur les gros scans unknown.
- Les modes exposables à l'UI sont `Eco`, `Normal`, `Performance`, `Max` ; `Auto` choisit un profil à partir des coeurs logiques et de la mémoire disponible.
- Toute parallélisation de scan doit découper par régions/chunks, accepter `CancellationToken`, reporter la progression, et ne jamais envoyer une masse non paginée de candidats au frontend.

### Gros fichiers à connaître

Ce sont les points de collision entre les 3 agents. Vérifier leur taille réelle avant d'annoncer un chiffre :
`(Get-Content <fichier> | Measure-Object -Line).Lines`

- `apps/desktop/application_controller.cpp` : **~10 000 lignes** — c'est LE fichier central
  - Contient : Smart Search, scan dispatch, write/freeze, profils, undo, debugging, AOB/patches
  - ⚠️ Les éditions `replace_in_file` peuvent échouer sur ce fichier (utiliser PowerShell pour les remplacements complexes)
- `ui/src/views/ExpertView.vue` : **~5 300 lignes**, 13 panneaux, 100+ boutons
  - En cours d'extraction vers `ui/src/components/expert/` — annoncer le chantier avant d'y toucher
- `ui/src/stores/app.ts` : **~5 000 lignes**, état global Pinia partagé par toutes les vues
- `core/candidates/candidate_store.cpp` : gestion file-backed des candidats
- `core/scanner/scan_engine.cpp` : moteur de scan (exact + multi-type)

### Problèmes connus (gotchas)
1. **Encodage Windows** : les fichiers peuvent avoir des fins de ligne CRLF. Si `replace_in_file` échoue, utiliser un script PowerShell — mais **toujours avec `-Encoding utf8`**. Un script sans encodage explicite produit du double-encodage (`é` → `Ã©`) qui s'affiche tel quel dans l'UI. C'est déjà arrivé sur `ExpertView.vue` (199 séquences corrompues, réparées le 16/08/2026).
2. **Fichier `application_controller.cpp` très gros** : l'outil `read_file` ne lit que les 1000 premières lignes. Pour lire plus loin, utiliser PowerShell (`Get-Content` + indexation)
3. **Build cache** : `build/CMakeCache.txt` — si CMake ne détecte pas les nouveaux fichiers, supprimer le dossier `build/` et relancer `configure.ps1`
4. **Shell** : le shell par défaut est PowerShell, pas cmd. Les commandes `cmd /c` peuvent être nécessaires pour la syntaxe batch
5. **Logger test** : `LoggerTest.LevelFiltering` échoue parfois (problème de permissions fichier temp Windows) — c'est préexistant, pas bloquant

## Source of truth

- **`KILLENGINE_PROJECT_SPEC.md`** : spécification complète du projet (phases, features, contrats)
- **`docs/PHASE_TRACKER.md`** : checklist d'avancement par phase (mettre à jour après chaque travail)
- **`docs/ULTIMATE_PRODUCT_GUIDELINE.md`** : direction produit premium, consignes agents, phases ultimes Assistant/Investigation/Trainer
- **`docs/USER_GUIDE.md`** : guide utilisateur final

## Workflow recommandé pour une nouvelle session IA

1. **Lire ce fichier** (`AGENTS.md`) pour le contexte
2. **Lire `docs/PHASE_TRACKER.md`** pour voir l'avancement et les tâches restantes
3. **Identifier les fichiers concernés** via `list_files` ou `search_files`
4. **Utiliser `read_file`** sur les petits fichiers ; **PowerShell** pour les gros fichiers (>1000 lignes)
5. **Implémenter** avec `write_to_file` (nouveaux fichiers) ou `replace_in_file` (éditions ciblées)
6. **Builder** avec `.\scripts\build.ps1`
7. **Tester** avec `.\build\bin\killengine_unit_tests.exe`
8. **Mettre à jour** `docs/PHASE_TRACKER.md` après complétion

## Conventions de code

- **Noms de fichiers** : `snake_case.cpp` / `snake_case.h`
- **Classes** : `PascalCase` (ex: `ApplicationController`, `CandidateStore`)
- **Méthodes** : `camelCase` (ex: `startExactScan`, `sortByConfidence`)
- **Enums** : `PascalCase` avec valeurs `PascalCase` (ex: `ValueType::Int32`)
- **Namespaces** : `killcore` (moteur), `killai` (IA), `killengine` (app)
- **Commentaires** : français dans le code applicatif, anglais dans les headers core
- **Logging** : `KE_LOG_INFO() << "message" << variable;`
- **Qt** : utiliser `QVariantMap` pour les réponses au frontend, `QString` partout

## Roadmap (Phase 13+)

Voir `docs/PHASE_TRACKER.md` section "PHASE 13". Les éléments restants :
- Passe de régression manuelle V1 (KillEngineTestTarget + application tierce autorisée)
- Optimisations futures potentielles :
  - Profil de performance adaptatif (`Auto`, `Eco`, `Normal`, `Performance`, `Max`)
  - Propagation de la confiance à travers `nextScan`
  - Bonus module connu dans `candidate_confidence`
  - Scan multi-type asynchrone (worker thread)
  - Tri UI dynamique (adresse vs confiance)
