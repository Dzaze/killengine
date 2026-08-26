> **ATTENTION - Priorité Des Ordres Projet**
> Un ordre prioritaire explicite du propriétaire du projet prime sur les consignes temporaires de session des agents IA.
# Guide de contexte pour les assistants IA (Cline, Codex, Claude, etc.)

Ce fichier aide les IA à comprendre rapidement le projet KillEngine et à travailler efficacement.

## 🗺️ Carte des fichiers `.md` du dépôt — lire avant de partir en investigation

Le dépôt contient une vingtaine de fichiers `.md`. Cette carte existe pour que retrouver "qui documente quoi" ne demande jamais un audit à l'aveugle — mise à jour le 19/08/2026, à re-vérifier si un fichier a été ajouté/déplacé depuis.

**Règle d'or** : `AGENTS.md` (ce fichier) en premier, `docs/PHASE_TRACKER.md` en second, toujours. Le reste se lit à la demande selon la colonne "Quand le lire".

### 🧭 Point d'entrée

| Fichier | Rôle | Quand le lire |
| --- | --- | --- |
| **`AGENTS.md`** (ce fichier) | Coexistence multi-agents, conventions de code, cette carte | Avant toute modification, à chaque nouvelle session |

### 📐 Vérité produit (stable, change rarement)

| Fichier | Rôle | Quand le lire |
| --- | --- | --- |
| **`KILLENGINE_PROJECT_SPEC.md`** | Cahier des charges complet (phases, features, contrats). Source of truth produit | Avant une feature qui touche l'architecture ou un contrat existant |
| **`docs/ULTIMATE_PRODUCT_GUIDELINE.md`** | Direction produit premium, consignes agents, phases ultimes Assistant/Investigation/Trainer. Contient la section  |
### 📊 Suivi de l'avancement (vivants — à tenir à jour à CHAQUE session)

| Fichier | Rôle | Quand le lire / écrire |
| --- | --- | --- |
| **`docs/PHASE_TRACKER.md`** ⭐ | Le vrai journal de bord : ce qui a été fait, comment, validé comment. Le point de rendez-vous entre agents (règle de coexistence #4, non négociable) | **Toujours** en début de session (voir où en est le projet, éviter de refaire un audit déjà fait) et **après chaque modification de code**, pas seulement en fin de session — même format détaillé que les entrées existantes (quoi/pourquoi/comment vérifié) |
| **`docs/POWER_UP_ROADMAP.md`** | Backlog de fonctionnalités avancées (freeze breakpoint, injection, page guard, scripting...) avec statut par section | Avant de proposer "quoi faire ensuite" — section "Prochains gros chantiers" en fin de fichier tient la liste vérifiée. **Piège déjà arrivé** : les sections peuvent dériver (dire "à faire" alors que c'est livré) si personne ne les recroise avec le code — toujours revérifier dans le code avant de faire confiance à une case cochée, pas seulement lire le texte |

### 🧠 Réflexion / stratégie

| Fichier | Rôle | Quand le lire / écrire |
| --- | --- | --- |
| **`docs/STRATEGY_ROOM.md`** | Hypothèses en cours de test, pistes explorées puis abandonnées (et pourquoi), anticipation de risques, idées pas encore assez mûres pour `POWER_UP_ROADMAP.md`. Le "brouillon" avant que quelque chose devienne une entrée de tracker | Avant de ré-explorer une piste (vérifier qu'elle n'a pas déjà été tranchée) ; pendant l'investigation d'un problème difficile qui mérite de garder trace du raisonnement, pas seulement du résultat |
| **`docs/KILLENGINE_CLR_TEST_TARGET_SPEC.md`** | Cahier des charges de `KillEngineClrTestTarget` (`tests/clr_targets/`), la cible de test CLR dédiée au candidat #8 de `POWER_UP_ROADMAP.md` (ClrMD/SOS) — projet .NET séparé, pas construit par CMake | Avant de toucher au graphe d'objets managés de test ou à sa surface pilotable par pipe |
| **`docs/KILLENGINE_CLR_INSPECTOR_SPEC.md`** | Spécification du MVP ClrMD (`tools/clr_inspector/KillEngineClrInspector`, helper .NET + pipe JSON-RPC) — statut fonctionnel livré, limitations connues, hors-scope explicite | Avant toute évolution du helper ClrMD, avant de brancher cette capacité sur `ApplicationController`/l'UI |

### 👤 Utilisateur final

| Fichier | Rôle | Quand le lire |
| --- | --- | --- |
| **`docs/USER_GUIDE.md`** | Guide utilisateur V1 : attacher, Assistant, profils, Expert, Trainer, paramètres, dépannage | Avant de documenter une fonctionnalité côté utilisateur, ou si l'UX d'un flux change |
| **`SC2.md`** | Conseils pratiques pour scanner StarCraft 2 spécifiquement (profondeur de scan Unknown, type de valeur recommandé) | Si la tâche concerne SC2 précisément |

### ✅ QA / validation

| Fichier | Rôle | Quand le lire |
| --- | --- | --- |
| **`docs/V1_REGRESSION_CHECKLIST.md`** | Checklist de non-régression avant de taguer une release candidate | Avant une release |
| **`docs/U1_AUTO_RESOLVE_VALIDATION.md`** | Plan et statut de validation manuelle du workflow Auto Resolve (Assistant proactif) | Si on retouche `startAutoResolve`/le workflow proactif |
| **`docs/PERFORMANCE_BENCHMARKS.md`** | Comment lancer/lire le benchmark de perf reproductible (scan engine, gros volumes de candidats) | Si on retouche le scan engine ou le stockage de candidats |
| **`docs/manual-validation-results/*.md`** | Logs de sessions de validation manuelle passées, un fichier daté par run | Référence historique — ne pas éditer rétroactivement, ajouter un nouveau fichier daté pour un nouveau run |

### 📦 Build / packaging / release

| Fichier | Rôle | Quand le lire |
| --- | --- | --- |
| **`README.md`** (racine) | Vue d'ensemble, build rapide, structure du dépôt, les 4 vues | Premier contact avec le projet |
| **`packaging/README.md`** | Build du package portable Windows (`package-windows.ps1`) | Avant un packaging/une release |
| **`docs/CODE_SIGNING.md`** | ⚠️ **Pour un humain, pas pour un agent** — obtenir un certificat Authenticode est une démarche d'achat/identité. Le tooling (`scripts/codesign.ps1`) est prêt, il attend juste le certificat | Ne rien tenter ici ; savoir juste que ça existe si la question de signature surgit |
| **`model/README.md`** (+ `model/assistant/`, `model/auto_resolver/`, `model/qwen/README.md`) | Convention du dossier des agents IA embarqués (manifests JSON, poids GGUF partagés) | Si la tâche touche le tool-calling IA local ou l'ajout d'un agent embarqué |
| **`docs/KILLENGINE_KERNEL_DRIVER_ARCHITECTURE.md`** | Architecture préparatoire du connecteur kernel : pont user-mode probe-only, contrat IOCTL minimal, garde-fous et non-objectifs | Avant toute discussion ou modification touchant un éventuel driver noyau |

### 🗄️ Historique / legacy — **ne pas prendre pour l'état actuel du projet**

| Fichier | Rôle | Piège à éviter |
| --- | --- | --- |
| **`message entre ia.md`** | Log de session ad-hoc du 09/08/2026, antérieur à la mise en place de `docs/PHASE_TRACKER.md` | Superseded — ne plus y écrire, `PHASE_TRACKER.md` est le seul journal à jour |
| **`docs/SC2_IMPROVEMENT_ANALYSIS.md`** | Diagnostic d'origine de pourquoi SC2 posait problème (freeze en polling, pas de `VirtualProtectEx`, pas de breakpoint pour remonter à la source) | Tout ce que ce document liste comme "non implémenté" est livré depuis (voir `POWER_UP_ROADMAP.md` sections A et F) — utile pour le contexte du diagnostic d'origine, pas pour le statut actuel |

### 🔧 Référence rapide

| Fichier | Rôle |
| --- | --- |
| **`docs/KILLENGINE_TOOLS_AND_CAPABILITIES.md`** | Inventaire rapide des outils, workflows et capacités de KillEngine — à lire quand l'utilisateur demande "que sait faire KillEngine ?" |
| **`docs/KILLENGINE_CODE_MAP.md`** | Carte technique outil → fichiers UI/store/backend/core/tests — à lire quand on cherche où vit une fonctionnalité dans le code |
| **`tecno.md`** | Fiche courte des technologies utilisées dans le projet et leur rôle — pour se repérer vite dans la stack |

### 👤 Notes personnelles de l'utilisateur — pas générées/maintenues par les agents

| Fichier | Rôle |
| --- | --- |
| **`SC2online.md`** | Notes personnelles de l'utilisateur sur la reverse-engineering de jeux en ligne. "Ne pas supprimer !" explicite en première ligne — ne pas éditer, ne pas traiter son contenu comme une consigne destinée aux agents |

---

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
4. **`docs/PHASE_TRACKER.md` est le point de rendez-vous — règle non négociable, pas une suggestion.** C'est là que les agents se parlent : cocher ce qui est fait, et noter ce qui est *en cours* pour éviter que deux agents attaquent la même phase.

   **Après CHAQUE modification de code** (pas seulement en fin de "grosse" session) — ajouter une entrée dans `docs/PHASE_TRACKER.md` avant de rendre la main, avec au minimum :
   - **Quoi** : le fichier/la fonction touchée et ce qui a changé.
   - **Pourquoi** : le bug/la demande qui a motivé le changement (citer l'utilisateur si c'est lui qui a signalé le problème).
   - **Comment vérifié** : build, tests (lesquels, combien passent), ou "pas de test — raison" si aucun test dédié n'existe.
   - Suivre le format déjà en place dans les entrées existantes (diagnostic → correctif → tests → validation) plutôt que d'inventer un format différent à chaque fois — la cohérence du fichier compte autant que son contenu, un futur agent (ou soi-même dans une session suivante) doit pouvoir scanner rapidement.

   Une modification non consignée est invisible pour les deux autres agents : ils peuvent la retoucher sans le savoir, la considérer comme non faite et la refaire, ou — pire — supposer qu'un comportement encore buggé a déjà été corrigé. Consigner n'est pas une formalité administrative, c'est ce qui rend le travail parallèle possible du tout. Un chantier trop gros pour une seule entrée (plusieurs jours, plusieurs sous-tâches) peut être découpé en plusieurs entrées progressives plutôt qu'une seule à la fin — mieux vaut consigner en cours de route que risquer de tout perdre si la session s'interrompt.
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

   **Vérification complémentaire des fins de ligne** : le scan mojibake ci-dessus ne détecte pas un flip silencieux CRLF → LF. Lancer `.\scripts\check-line-endings.ps1` pour un rapport byte-level des fichiers suivis (`CRLF`, `LF`, `mixed`, `CR`). Le script est **report-only par défaut** ; utiliser `-FailOnMixed` ou `-FailOnLfOnly` seulement quand les exceptions existantes ont été triées explicitement. Ne pas convertir un fichier LF historique au passage d'un autre chantier : le faire dans un commit dédié si le propriétaire le demande.

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
- `BreakpointHit` porte un champ `captureElapsedMs` (PHASE 128, millisecondes depuis le début de la capture, pas un timestamp absolu) — ajouté pour `display_source_classifier` ci-dessous, réutilisable par tout futur code qui a besoin de mesurer le rythme des hits d'une même capture.

### Heuristique "champ affiché vs champ source" (classifieur v1, PHASE 128)
- Module : `core/scanner/display_source_classifier.h/.cpp` (`classifyFieldStability` — logique pure testable sans process réel ; `classifyFieldStabilityLive` — capture réelle via `findWhatWrites` ci-dessus, mêmes précautions "adresse chaude").
- Objectif : donner suite à `docs/STRATEGY_ROOM.md` (20/08/2026, anecdote XP Solitaire "Bulles") sans deviner à partir d'un seul cas — voir la cible synthétique PHASE 118 (`g_counterSource`/`g_counterCurrent`/`g_counterDisplayed` dans `tests/memory_targets/test_target_main.cpp`) qui reproduit fidèlement le motif "champ recalculé à chaque tick par interpolation".
- Approche **dynamique/passive**, pas de reconnaissance statique de motifs de désassemblage : aucune écriture injectée sur la cible, juste classification du RYTHME et de la SOURCE (instruction) des écritures déjà captées par `findWhatWrites`. Un champ réécrit à intervalle régulier par une seule instruction dominante → `LikelyDerivedDisplay` (probablement un champ affiché interpolé, geler/patcher directement ne tiendra probablement pas). Un champ réécrit de façon irrégulière et/ou par plusieurs instructions différentes → `LikelyEventDriven`. Zéro écriture captée → `NoWritesObserved`, volontairement honnête plutôt que de deviner "stable" (la fenêtre de capture peut juste avoir été trop courte).
- **Branché à l'Assistant en lecture seule (PHASE 131)** : `ApplicationController::analyzeFieldStability(addressHex, options)` (Q_INVOKABLE, wrapper direct de `classifyFieldStabilityLive`) + fast-path `ai/ai_engine.cpp::matchFieldStabilityTool` (mots-clés FR/EN "champ affiché"/"vraie source"/"displayed field"/etc., outil `analyze_field_stability` dans `ai/tool_registry.cpp`, `risk=debug` mais `requiresConfirmation=false` car jamais d'écriture). Pas de bouton UI dédié (collision risk sur `ExpertView.vue`, 5200+ lignes) — l'action Assistant seule couvre la demande. Piège retrouvé : une question avec une adresse `0x...` était happée par le pré-intent `ActivateMemoryTargets` de `startSmartSearch` avant d'atteindre le fast-path (même piège que `trainer`, PHASE 129) — corrigé via un flag `smartSearchFieldStabilityQuery` en `&&` sur les 8 points de bypass existants.
- Tests : `killengine_unit_tests.exe --gtest_filter=DisplaySourceClassifier.*` (logique pure), `killengine_integration_tests.exe --gtest_filter=DisplayVsSourceTargetTest.ClassifyLive*` (capture réelle), `AIEngineContextualFallbackTest.FieldStabilityFastPath*` (fast-path Assistant FR/EN).

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

### Connecteur d'automatisation locale (pilotage agent IA en temps réel)
- Module : `apps/desktop/automation_pipe_server.h/.cpp`.
- Objectif : demandé explicitement par l'utilisateur le 19/08/2026 pour des sessions de test live sur un vrai jeu (ex. Solitaire) — un agent IA doit pouvoir piloter KillEngine (attacher, scanner, écrire, watch...) en même temps que l'utilisateur observe/agit dans l'UI Qt, sans que l'agent ait besoin de contrôle souris/écran.
- Fonctionnement : `QLocalServer` (named pipe Windows via `Qt6::Network`, déjà lié) écoute sur `\\.\pipe\KillEngineAutomationPipe`, dans le **même processus** que l'UI (même `ApplicationController`, même état partagé `m_attached`/`m_candidates`/etc. — pas une deuxième implémentation du moteur). Protocole JSON-RPC minimal, une ligne par requête/réponse : `{"id":1,"method":"attachProcess","params":[12345]}` → `{"id":1,"result":true}`.
- **Dispatch générique par réflexion `QMetaMethod`**, pas de wrapper écrit à la main par méthode : `method` doit correspondre exactement au nom d'une méthode `Q_INVOKABLE` d'`ApplicationController`, les `params` JSON sont convertis vers les types Qt attendus via `QVariant::convert`. Conséquence directe : **toute nouvelle méthode `Q_INVOKABLE` devient automatiquement pilotable via ce pipe sans y toucher** — à garder en tête en ajoutant une méthode sensible à `ApplicationController`, elle est exposée ici aussi par construction.
- **🎯 Convention (décidée le 26/08/2026, à appliquer à chaque nouvelle capacité) : le pipe d'automatisation est le canal de contrôle universel du moteur — n'importe quel outil ajouté à `ApplicationController` doit y rester joignable, et ça ne doit pas rester "vrai en théorie par réflexion" seulement.** Concrètement, quand une capacité significative est ajoutée ou change de comportement interne (ex. PHASE 122 : fallback relais PowerShell dans `applyCodePatch`/`restoreCodePatch`, totalement transparent pour l'appelant pipe) :
  1. **Valider par un vrai appel pipe**, pas juste supposer que la réflexion suffit — c'est ce qui a permis de prouver en direct le déclenchement du fallback EDR de PHASE 122 (`errorCode=5` côté `KillEngine.exe` réel, puis succès via le relais), voir `docs/PHASE_TRACKER.md` PHASE 122.
  2. Si l'outil est du genre à être rappelé souvent en session de test live, ajouter un wrapper de confort dans `scripts/killengine.lua` (`ke.xxx`, à côté de `ke.attach`/`ke.scan_exact`/etc.) — un script Lua passe déjà par `ke.call(method, params)` → `automation-pipe-call.ps1` en interne, donc **n'importe quelle méthode `Q_INVOKABLE` y est déjà accessible sans wrapper dédié** ; le wrapper n'est qu'un confort de lisibilité, pas une condition d'accès.
  3. Documenter l'exemple concret (pas seulement le principe général) dans l'entrée `docs/PHASE_TRACKER.md` de la phase concernée, pour que la découvrabilité avance au même rythme que le code plutôt que de rester une note oubliée.
  - **⚠️ Piège vérifié en direct le 26/08/2026 : un script Lua ne peut rappeler le pipe (`ke.call`) que s'il est lancé via `executeLuaScriptAsync`, jamais via `executeLuaScript` (le synchrone).** `executeLuaScript` bloque le thread qui traite déjà la requête pipe/QML en cours jusqu'à ce que `lua.exe` se termine ; si le script fait un `ke.call` nested pendant ce temps, la connexion pipe imbriquée ne peut jamais être servie tant que ce même thread reste bloqué → deadlock garanti, résolu uniquement par le timeout du script (`options.timeoutMs`), avec 0 octet de stdout capturé. `executeLuaScriptAsync` lance `lua.exe` sur un `std::thread` séparé et retourne `started:true` immédiatement, ce qui laisse le thread principal libre de servir la connexion imbriquée — testé en direct avec `ke.apply_code_patch`/`ke.restore_code_patch` imbriqués : les deux réussissent, chacun avec ~1s de latence réelle (démarrage à froid du `powershell.exe` imbriqué par appel, pas un coût négligeable si le script fait beaucoup d'allers-retours). **Donc : tout script Lua qui doit rappeler KillEngine doit être lancé en async, point final** — à rappeler explicitement si un futur agent écrit un script Lua qui utilise `ke.call`/`ke.apply_code_patch`/etc.
  - **Ne pas confondre avec `callVueStoreAction`** (`apps/desktop/application_controller.cpp`, PHASE 119) : ce pont-là est une liste blanche **volontairement restreinte** aux actions de store Pinia (`keepCandidate`/`ignoreCandidate`/`addAddressToWatch`) pour piloter l'état UI depuis un script Lua — il ne donnera jamais accès à des méthodes moteur comme `applyCodePatch`. Le canal pertinent pour "tout outil moteur doit être pilotable" est le pipe d'automatisation (et son wrapper Lua générique `ke.call`), pas ce pont-là.
- **Désactivé par défaut** : n'écoute que si la variable d'environnement `KILLENGINE_AUTOMATION_PIPE=1` est présente au lancement (voir `main.cpp`). Ne jamais l'activer dans un build livré à l'utilisateur final — outil dev/test uniquement.
- **Aucune authentification** : le named pipe est déjà local-machine-only par construction Windows, accepté comme risque pour un outil dev/test local.
- **⚠️ Bypass volontaire du RiskGate** : les confirmations write/freeze/debug/patch vivent côté frontend (`confirmRiskAction`, `ui/src/stores/app.ts`), pas dans `ApplicationController`. Un appel via ce pipe exécute donc l'action **immédiatement**, sans dialogue de confirmation. Ce n'est pas "l'UI sans les boutons", c'est un accès direct au moteur — à n'activer que pendant une session supervisée par un humain.
- Audit : chaque appel pipe est loggé dans `scan_telemetry.jsonl` via `logAiAudit('automation_pipe_call', {method, argCount, success, error?})` — même flux que le reste de la télémétrie, pas un canal séparé.
- Client : `scripts/automation-pipe-call.ps1 -Method <nom> -ParamsJson '[...]'` (une requête, une réponse, ferme la connexion). **Piège PowerShell découvert en l'écrivant** : `ConvertFrom-Json '["x"]'` (tableau JSON à un seul élément) retourne un `string` nu, pas un tableau — `ConvertTo-Json` le resérialise donc comme scalaire au lieu de `[...]`, ce qui casse le comptage d'arguments côté dispatcher. Corrigé avec `@($paramsObject)` pour forcer le recasting en tableau avant `ConvertTo-Json`. À connaître pour tout futur script PowerShell qui fait un aller-retour JSON par ce genre de cmdlet.
- Validé en conditions réelles (pas juste compilé) le 19/08/2026 : `KillEngine.exe` lancé avec la variable d'env, log confirmant l'écoute, puis appels réels via le script client couvrant les principaux profils de signature (`ping` — QString param+retour, `getVersion` — sans param, `getProcesses` — retour `QVariantList`, `attachProcess` — param `int`/retour `bool`, `getSettings` — retour `QVariantMap`, `detachProcess` — retour `void`), tous confirmés dans `scan_telemetry.jsonl`.
- Statut : pas de test automatisé dédié — `ApplicationController`/`AutomationPipeServer` ne sont compilés que dans la cible `KillEngine.exe` (`apps/desktop/CMakeLists.txt`), pas liés aux binaires `tests/`, donc pas testables depuis les suites GTest sans restructurer le build. Validation par smoke-test réel documentée ci-dessus et dans `docs/PHASE_TRACKER.md`.

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
6. **Piège grave découvert le 17/08/2026 — rebuild incrémental après modif d'un `.h`** : ajouter ou changer un membre dans `application_controller.h` (ou tout header inclus par plusieurs `.cpp`) peut laisser `ninja` **ne pas recompiler** certains `.cpp` qui dépendent pourtant de ce header (constaté sur `main.cpp.obj`, resté à l'ancien timestamp alors que `application_controller.cpp.obj` était à jour — la règle ninja générée pour `main.cpp` est taguée `..._unscanned_...`, signe que ses dépendances d'en-têtes n'ont jamais été correctement enregistrées dans `.ninja_deps`). Résultat : deux `.obj` compilés contre deux tailles/layouts différents de la même classe, liés ensemble sans erreur — **crash aléatoire au démarrage** (`STATUS_HEAP_CORRUPTION`, `0xc0000374`), pas une erreur de compilation, donc rien ne prévient. Après toute modif de layout d'un header partagé (nouveau membre, nouvelle méthode dans la classe), avant de faire confiance à un build qui rapporte "Build successful" : `touch apps\desktop\*.cpp` (ou équivalent PowerShell `(Get-Item ...).LastWriteTime = Get-Date`) puis rebuild, pour forcer la recompilation de tous les `.cpp` du même dossier plutôt que de se fier à la détection incrémentale de ninja.
7. **Piège découvert le 18/08/2026 — mauvais `ninja` sur le `PATH` fait planter `build.ninja` avec une erreur qui ressemble à une corruption** : sur cette machine, `C:\tizen-studio\tools\ninja.exe` (version 1.5.3, très ancienne) passe avant le `ninja` pip installé (`python -c "import ninja; print(ninja.BIN_DIR)"`, version 1.13) sur le `PATH` par défaut. Invoquer `ninja` directement (ou tout `cmake --build` qui ne force pas explicitement le `PATH`) tombe sur `ninja: error: build.ninja:485: expected ':', got '|' ($ also escapes ':')` sur les règles `gmock_autogen`/`gtest` générées par CMake — l'ancien ninja gère mal l'expansion de `${cmake_ninja_workdir}` (qui contient `C$:\MES$ APPS$ DEV\...`, échappement du chemin avec espaces + lettre de lecteur) dans la liste des outputs d'une règle. Ce n'est **pas** un `build/` corrompu, ne pas le supprimer pour "réparer" — l'erreur disparaît complètement avec le ninja pip en tête de `PATH` (exactement ce que fait déjà `scripts\build.ps1` en préfixant `$ninjaPath`). Si une invocation manuelle de `ninja`/`cmake --build` échoue avec cette erreur précise, vérifier `where ninja` avant de creuser plus loin.

## Source of truth

Voir la « Carte des fichiers `.md` du dépôt » tout en haut de ce fichier — table complète par catégorie (spec produit, avancement, réflexion/stratégie, QA, legacy...), pas seulement les 4 fichiers historiquement listés ici.

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

### Formatage (mesuré sur le dépôt le 17/08/2026 — à respecter, pas une préférence)

Un `.editorconfig` à la racine applique une partie de ces règles automatiquement dans les éditeurs qui le lisent (VS Code le fait nativement). Il ne dispense pas de vérifier : un agent qui écrit via un script (PowerShell, patch, etc.) plutôt que via l'éditeur ne bénéficie pas de l'auto-application.

| | C++ (`core/`, `apps/`, `ai/`) | TS / Vue (`ui/src/`) | PowerShell (`scripts/`) |
| --- | --- | --- | --- |
| Indentation | 4 espaces, jamais de tab | 2 espaces, jamais de tab | 4 espaces, jamais de tab |
| Fin de ligne | **CRLF cible** ; vérifier avec `scripts/check-line-endings.ps1` | **CRLF cible** | **CRLF cible** |
| Encodage | UTF-8 **sans BOM** (voir section coexistence ci-dessus) | UTF-8 sans BOM | UTF-8 sans BOM |
| Accolades | ouvrante sur la même ligne (`if (...) {`, `void f(...) {`) | — | — |
| Chaînes | `QString`, guillemets doubles côté C++ | guillemets **simples** (`'texte'`) | — |
| Point-virgule | requis (C++) | **absent** — le style du repo n'en met pas en fin d'instruction TS/Vue, ne pas en réintroduire | — |
| Ligne finale | fichier terminé par un saut de ligne | idem | idem |

Ne pas changer ces conventions « au passage » sur un fichier existant (ex. ne pas repasser un `.vue` en 4 espaces ou ajouter des points-virgules) : ça pollue le diff et gêne les autres agents qui relisent le fichier ensuite.

### Astuces pratiques pour les agents IA (Cline/z.ai, Codex, etc.) qui écrivent via PowerShell

Au-delà de l'encodage (déjà traité en détail plus haut), les erreurs de formatage les plus fréquentes viennent de PowerShell utilisé comme mécanisme d'écriture de fichier plutôt que d'un outil d'édition dédié :

1. **Préférer l'outil d'édition natif de l'agent (diff/patch ciblé) à un script PowerShell** quand c'est possible. Un script qui réécrit tout le fichier perd le CRLF, l'indentation ou l'encodage d'origine si l'un des points ci-dessous est oublié.
2. **Si un script PowerShell doit écrire du texte**, forcer explicitement fin de ligne + encodage :
   ```powershell
   $content = $content -replace "`r`n", "`n" -replace "`n", "`r`n"   # normaliser en CRLF
   Set-Content -Path $f -Value $content -Encoding utf8NoBOM -NoNewline
   ```
   Ne jamais laisser PowerShell choisir l'encodage ou la fin de ligne par défaut.
3. **Here-strings** : utiliser la forme **simple-quotée** `@'...'@` (littérale) pour injecter du code contenant `$`, des backticks ou des guillemets doubles. La forme `@"..."@` interpole ces caractères et corrompt silencieusement le contenu (variables shell expansées, backticks avalés).
4. **Ne jamais mélanger tabs et espaces** dans un remplacement `-replace` ou un here-string : PowerShell ne convertit pas automatiquement, le tab inséré reste un vrai `\t` invisible dans le diff mais visible pour les autres outils (linters, autres agents).
5. **Vérifier après coup**, pas seulement avant de committer : `Select-String -Pattern "`t" -Path <fichier>` pour détecter une tabulation introduite par erreur, et la commande de vérification d'encodage donnée plus haut (section coexistence, point 7).
6. **Édition ciblée uniquement** : pour un patch sur un fichier existant, lire le bloc exact à remplacer et ne toucher que lui (cf. règle 2 de la section coexistence) — une réécriture complète via PowerShell est la manière la plus courante de perdre silencieusement le CRLF/l'indentation du reste du fichier.

## Roadmap (Phase 13+)

Voir `docs/PHASE_TRACKER.md` section "PHASE 13". Les éléments restants :
- Passe de régression manuelle V1 (KillEngineTestTarget + application tierce autorisée)
- Optimisations futures potentielles :
  - Profil de performance adaptatif (`Auto`, `Eco`, `Normal`, `Performance`, `Max`)
  - Propagation de la confiance à travers `nextScan`
  - Bonus module connu dans `candidate_confidence`
  - Scan multi-type asynchrone (worker thread)
  - Tri UI dynamique (adresse vs confiance)
