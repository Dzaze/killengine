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
- Après une capture unknown, l'utilisateur choisit le type de comparaison via les boutons guidés (`ça augmente`, `ça diminue`, `ça change`, `stable`).
- `stable` / `unchanged` ne doit pas être proposé ni accepté en première comparaison unknown : il garde trop de mémoire et peut saturer les gros scans. Il sert surtout après une première réduction.
- Les comparaisons unknown doivent rester bornées côté moteur pour éviter les retours massifs et les blocages UI.

### Pointer Chains
- Les chaines de pointeurs sont représentées par `core/pointer/PointerChain` et peuvent être stockées dans les profils via `LocatorKind::PointerChain`.
- Convention de résolution : chaque offset suit un déréférencement (`read pointer`, puis `+ offset`), y compris le dernier offset.
- Les nouvelles méthodes C++ exposées à l'UI doivent aussi être ajoutées dans `ui/src/services/backend.ts`.

### Gros fichiers à connaître
- `apps/desktop/application_controller.cpp` : **~3700 lignes** — c'est LE fichier central
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
  - Propagation de la confiance à travers `nextScan`
  - Bonus module connu dans `candidate_confidence`
  - Scan multi-type asynchrone (worker thread)
  - Tri UI dynamique (adresse vs confiance)
