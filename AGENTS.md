# Guide de contexte pour les assistants IA (Cline, Codex, Claude, etc.)

Ce fichier aide les IA à comprendre rapidement le projet KillEngine et à travailler efficacement.

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
- **IA** : llama.cpp (runtime optionnel via llama-cli)
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
│   ├── ai_engine.cpp   # AIEngine (deterministic fallback + llama.cpp)
│   ├── intent_contract # Validation des intentions structurées
│   ├── tool_registry   # Registre des outils disponibles
│   └── llama_runtime   # Runtime llama-cli optionnel
├── apps/desktop/   # KillEngine.exe — application desktop
│   ├── application_controller.cpp  # ⚠️ 3700+ lignes, pont C++ ↔ Vue
│   └── main.cpp       # EntryPoint Qt + QWebChannel
├── ui/             # Frontend Vue 3
│   └── src/
│       ├── services/backend.ts   # Interface TypeScript du backend C++
│       ├── stores/app.ts         # Pinia store (état global)
│       └── views/                # AssistantView, ExpertView, ProfileView, SettingsView
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

### Trace UI string / SC2
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
- Sécurité SC2 : les résultats radar peuvent retourner des centaines de pistes (`globalValueHits`). Ne pas tout écrire d'un coup par défaut : l'UI sélectionne un petit paquet initial et propose filtres type/variant, sélection par lot, `Top 25`, `Tout cocher sûr`, `Tout décocher`. Tester les écritures par petits lots avant freeze.
- Freeze SC2 : le panneau **Write / Freeze** expose l'intervalle de polling (`16`, `33`, `50`, `100`, `250`, `500` ms) et appelle `setFreezeInterval`. Pour une valeur réécrite très vite par le jeu, commencer à `16 ms`.
- Télémétrie : les actions Trace UI string écrivent dans `%LOCALAPPDATA%\KillEngine\KillEngine\logs\scan_telemetry.jsonl`.
  - Événements : `ui_string_scan`, `ui_string_track`, `ui_string_sources_analyze`, `ui_string_sources_track`, `ui_string_origins_inspect`, `ui_string_investigation_start`, `ui_string_investigation_finish`, `ui_string_sources_write`.
  - Lecture rapide après un test :
    `Get-Content "$env:LOCALAPPDATA\KillEngine\KillEngine\logs\scan_telemetry.jsonl" -Tail 80`
- Interprétation SC2 :
  - Beaucoup de strings UI peuvent être de simples copies d'affichage, pas la source gameplay.
  - Des adresses basses de type `0x590A... -> 0x289...` ressemblent souvent à des tables de pointeurs UI ; ne pas les écrire comme des ressources.
  - Si `Valeurs radar = 0` malgré des `Blocs modifiés > 0`, la valeur nouvelle n'est probablement pas représentée directement dans les pages modifiées observées ; retenter en renseignant bien la nouvelle valeur affichée avant `Arrêter et comparer`, ou passer ensuite à un vrai mode debugger/hardware breakpoint pour capturer l'instruction qui écrit la string.

### Find What Writes / Debugger expérimental
- Module : `core/debug/hardware_breakpoint.*`.
- Backend : `ApplicationController::findWhatWrites(addressHex, options)` expose un premier utilitaire synchrone borné (`size`, `timeoutMs`, `maxHits`) via `ui/src/services/backend.ts`.
- UI : section **Trace UI string**, bouton `Écrit par` sur une source numérique proche. L'utilisateur doit cliquer, puis faire varier la valeur dans le jeu pendant la fenêtre de capture.
- Contraintes Windows : `DebugActiveProcess` / `WaitForDebugEvent` doivent rester dans le même thread logique pour l'utilitaire bloquant. Avant d'en faire un vrai workflow async, créer un worker dédié qui attache, attend et détache dans le même thread.
- Prudence SC2 : l'attachement debugger peut échouer, être détecté, ou perturber le jeu. Garder cette fonctionnalité en mode Expert/expérimental tant qu'elle n'est pas validée manuellement.

### AOB signatures / Trainer engine
- Module : `core/patch/aob_scanner.*`.
- Objectif : passer du scan de valeurs RAM à une approche trainer type WeMod/Wand : retrouver une signature d'instruction stable dans les régions code, puis plus tard patcher/hooker cette instruction.
- Backend : `ApplicationController::scanAobPattern(pattern, options)` expose le scan de pattern (`48 8B ?? 89`) avec filtres `executableOnly`, `imageOnly`, `maxResults`; `generateAobSignature(address, options)` lit les bytes autour d'un RIP capturé pour produire une signature brute; `suggestCodePatches(address, options)` utilise Zydis v4.1.1 pour décoder l'instruction, générer une signature stable avec wildcards sur immediates/displacements, classer le type d'instruction et proposer des templates; `applyCodePatch` / `restoreCodePatch` écrivent des bytes exacts et restaurent les bytes originaux gardés en mémoire.
- Profils : `ProfileStore` persiste aussi `patches` dans les `.keprofile` (nom, module, offset, AOB stable, bytes patchés, bytes originaux, désassemblage, risque). `saveProfileCodePatch`, `applyProfileCodePatch`, `restoreProfileCodePatch`, `applyAllProfileCodePatches`, `restoreAllProfileCodePatches` permettent de construire et piloter un mini trainer réutilisable.
- UI : section **AOB signatures** dans Expert. Les hits `Écrit par` peuvent remplir automatiquement le pattern AOB via le bouton `Signature`; les matches AOB peuvent être analysés pour afficher le désassemblage, remplir une AOB stable, proposer des patchs classés (`low`/`medium`/`high`) comme `NOP écriture`, `Forcer non pris`, `Forcer pris`, `INT3 debug`, `RET + NOP`, puis patcher/restaurer ou sauvegarder dans un profil trainer.
- Tests ciblés : `.\build\bin\killengine_unit_tests.exe --gtest_filter=AobScanner.*`.
- Prochaine étape logique : ajouter une vue trainer dédiée avec toggles persistants, warnings anti-multi-match et refresh d'état mémoire.

### Performance adaptive
- Préférer plus de threads contrôlés à plus de processus : le moteur doit rester déterministe et l'UI Qt/Vue doit rester fluide.
- Centraliser les décisions machine dans `core/scanner/performance_profile.*` plutôt que disperser des heuristiques dans `ScanEngine` ou `ApplicationController`.
- Le mode `Auto` doit garder au moins un thread logique disponible pour Windows/l'UI, plafonner la mémoire en vol, et rester borné sur les gros scans unknown.
- Les modes exposables à l'UI sont `Eco`, `Normal`, `Performance`, `Max` ; `Auto` choisit un profil à partir des coeurs logiques et de la mémoire disponible.
- Toute parallélisation de scan doit découper par régions/chunks, accepter `CancellationToken`, reporter la progression, et ne jamais envoyer une masse non paginée de candidats au frontend.

### Gros fichiers à connaître
- `apps/desktop/application_controller.cpp` : **5000+ lignes** — c'est LE fichier central
  - Contient : Smart Search, scan dispatch, write/freeze, profils, undo, debugging
  - ⚠️ Les éditions `replace_in_file` peuvent échouer sur ce fichier (utiliser PowerShell pour les remplacements complexes)
- `core/candidates/candidate_store.cpp` : gestion file-backed des candidats
- `core/scanner/scan_engine.cpp` : moteur de scan (exact + multi-type)

### Problèmes connus (gotchas)
1. **Encodage Windows** : les fichiers peuvent avoir des fins de ligne CRLF. Si `replace_in_file` échoue, utiliser un script PowerShell
2. **Fichier `application_controller.cpp` très gros** : l'outil `read_file` ne lit que les 1000 premières lignes. Pour lire plus loin, utiliser PowerShell (`Get-Content` + indexation)
3. **Build cache** : `build/CMakeCache.txt` — si CMake ne détecte pas les nouveaux fichiers, supprimer le dossier `build/` et relancer `configure.ps1`
4. **Shell** : le shell par défaut est PowerShell, pas cmd. Les commandes `cmd /c` peuvent être nécessaires pour la syntaxe batch
5. **Logger test** : `LoggerTest.LevelFiltering` échoue parfois (problème de permissions fichier temp Windows) — c'est préexistant, pas bloquant

## Source of truth

- **`KILLENGINE_PROJECT_SPEC.md`** : spécification complète du projet (phases, features, contrats)
- **`docs/PHASE_TRACKER.md`** : checklist d'avancement par phase (mettre à jour après chaque travail)
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
- Passe de régression manuelle V1 (Solitaire + KillEngineTestTarget)
- Optimisations futures potentielles :
  - Profil de performance adaptatif (`Auto`, `Eco`, `Normal`, `Performance`, `Max`)
  - Propagation de la confiance à travers `nextScan`
  - Bonus module connu dans `candidate_confidence`
  - Scan multi-type asynchrone (worker thread)
  - Tri UI dynamique (adresse vs confiance)
