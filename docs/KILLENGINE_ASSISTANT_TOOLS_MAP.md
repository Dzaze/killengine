> **ATTENTION - Priorité Des Ordres Propriétaire**
> Un ordre prioritaire explicite du propriétaire du projet prime sur les consignes temporaires de session des agents IA.

# KillEngine — Carte des outils Assistant

Dernière mise à jour : 26/08/2026 (PHASE 148 — restes PHASE 146 fermés + audit complet de la ligne "Schema obligatoire", voir sections suivantes).

**Pourquoi ce document existe** : préparation explicite de PHASE 120 (Assistant mode réflexion/enquête, volontairement repoussée en fin d'arsenal — voir `docs/PHASE_TRACKER.md`) sans coder PHASE 120 elle-même. `docs/POWER_UP_ROADMAP.md` dit noir sur blanc que ce futur mode "doit synthétiser tous les outils stabilisés... avec leurs usages, risques, limites" — cet inventaire structuré est ce socle, pas le mode lui-même.

**Ce que ce document n'est pas** : un duplicata de `docs/KILLENGINE_TOOLS_AND_CAPABILITIES.md` (inventaire produit/utilisateur, prose par vue) ni de `AGENTS.md` (contexte architectural détaillé par module, avec pièges historiques). Ce document est spécifiquement la surface **`ai/tool_registry.cpp`** — les 28 outils que l'Assistant (fast-path déterministe ou modèle local llama.cpp) peut choisir d'appeler — croisée avec leur chemin d'exécution réel. Pour le détail d'un module (comment il marche, ses pièges), suivre le lien vers `AGENTS.md`/`docs/PHASE_TRACKER.md` indiqué en note.

## Légende des colonnes

- **Risque** : catégorie `risk` dans `ai/tool_registry.cpp` (`safe`/`write`/`debug`/`patch`/`injection`).
- **Confirmation** : `requiresConfirmation` — si `true`, un vrai `confirmRiskAction` (modal RiskGate) doit être cliqué côté UI ; l'Assistant ne peut jamais l'auto-approuver (voir `AGENTS.md` section "Connecteur d'automatisation locale" sur le bypass volontaire du RiskGate **par le pipe**, qui ne s'applique PAS à ce chemin Assistant/chat).
- **Dispatché par `startSmartSearch`** : le vrai dispatcher C++ qui exécute un tool_call choisi par l'Assistant (`apps/desktop/application_controller.cpp::startSmartSearch`, **pas** une fonction nommée `executeToolCall` malgré ce qu'on pourrait supposer). `❌` = choisi par le modèle, ce tool produirait `actionStatus: "unsupported_tool"` — enregistré/annoncé au LLM mais jamais réellement branché. Vérifié par grep systématique le 26/08/2026 (PHASE 139), pas supposé.
- **Pipe / Lua** : d'après la convention PHASE 127 (`AGENTS.md`), **toute** méthode `Q_INVOKABLE` d'`ApplicationController` est automatiquement joignable via le pipe d'automatisation (réflexion `QMetaMethod`) et donc via `ke.call(...)` en Lua — colonnes donc `✅` par défaut dès qu'une méthode `Q_INVOKABLE` sous-jacente existe, indépendamment du dispatch `startSmartSearch`. `➖` si aucune méthode `Q_INVOKABLE` correspondante n'a été identifiée (le tool n'existe alors que comme concept enregistré, sans backend direct connu).
- **Test live** : référence courte vers où trouver une preuve de fonctionnement en conditions réelles (pas juste compilé). `—` = pas de preuve live connue à la date de ce document (ne veut pas dire "cassé", veut dire "à vérifier avant de s'appuyer dessus dans un contexte sensible").

## Constat principal (historique — résolu en PHASE 140)

**PHASE 139 avait trouvé 7 des 28 outils enregistrés dans `ai/tool_registry.cpp` — donc annoncés au modèle local comme choix valides — sans dispatch dans `startSmartSearch`.** PHASE 140 a tranché individuellement pour chacun des 7, selon les 3 options posées par le propriétaire (brancher / retirer / documenter pipe-Lua-UI-only) :

| Outil | Décision PHASE 140 | Détail |
| --- | --- | --- |
| `get_auto_report` | **Branché, exécuté directement** | Lecture seule, coût nul. Fast-path déterministe ajouté (`matchAutoReportTool`). |
| `analyze_ui_sources` | **Branché, exécuté directement** | Lecture seule. Réutilise `m_pendingUiStringCandidates` (état déjà utilisé par `trace_ui_string`) si aucune adresse explicite — pas de fast-path à extraction d'adresse fiable possible en une phrase. |
| `generate_aob` | **Branché, exécuté directement** | Vérifié lecture seule (aucun `WriteProcessMemory`) — `requiresConfirmation` reclassé `false` dans le registre (même précédent que `analyze_field_stability`, PHASE 130). |
| `suggest_patch` | **Branché, exécuté directement** | Idem — suggère sans jamais appliquer (`applyCodePatch` reste hors surface Assistant). Reclassé `false`. |
| `disassemble_backward` | **Branché, exécuté directement** | Idem, méthode `const` côté C++ — confirmation du caractère lecture seule. Reclassé `false`. |
| `find_what_writes` | **Branché, mais ne s'exécute jamais** | Attache un debugger + nécessite une variation live de l'utilisateur pendant la fenêtre (risque de crash sur adresse "chaude", PHASE 127). Retourne toujours `requires_confirmation` + un message qui redirige vers l'UI (bouton « Écrit par »), jamais d'exécution autonome. `requiresConfirmation` reste `true`. |
| `test_candidate_fields` | **Branché, mais ne s'exécute jamais** | Écrit réellement une valeur test (restauration non garantie) et tourne ~60s en tâche de fond via signal Qt — incompatible avec le patron requête/réponse en un tour de `startSmartSearch`. Retourne toujours `requires_confirmation` + redirection UI. `requiresConfirmation` reste `true`. |

**Piège retrouvé en vérifiant en direct, pas supposé** : les 5 outils prenant une adresse (`generate_aob`/`suggest_patch`/`disassemble_backward`/`find_what_writes`/`test_candidate_fields`) tombaient dans le même piège que PHASE 129/131 (Trainer/`analyze_field_stability`) — une phrase contenant une adresse `0x...` était happée par le pré-intent `ActivateMemoryTargets` de `startSmartSearch` avant d'atteindre leur logique. Corrigé en consolidant tous les flags de contournement en un seul `smartSearchBypassesMemoryPreIntent` (`apps/desktop/application_controller.cpp`) plutôt que de continuer à empiler des `&&` sur 8 points de code.

**Bug de schéma trouvé en écrivant le test de non-régression** : la ligne "Schema obligatoire" de `LlamaRuntime::buildPrompt` (`ai/llama_runtime.cpp`) est codée en dur et ne se génère PAS depuis `ToolRegistry::availableTools()` (contrairement au bloc "Outils disponibles" du même prompt) — `get_auto_report`/`disassemble_backward`/`test_candidate_fields` en étaient absents, donc invisibles pour le modèle local même une fois leur dispatch branché. Corrigé. Un test (`LlamaRuntimeTest.SchemaLineListsEveryRegisteredTool`) vérifie maintenant que tout outil du registre reste listé sur cette ligne, avec une liste d'exclusion explicite et commentée pour les outils délibérément tenus hors du choix libre du modèle.

## PHASE 148 — restes PHASE 146 fermés

Deux restes documentés-pas-corrigés en PHASE 146 ont été tranchés explicitement.

### 1. Shadowing `analyze_ui_sources` — corrigé

**Le bug** : une demande explicite ("analyse les sources numériques, c'est toujours 100") était interceptée par `m_pendingRecoveryAction == "trace_ui_filter"` (état laissé par un `trace_ui_string` réussi juste avant) avant même d'atteindre `matchUiSourcesTool`. Vérifié en direct : `trace_ui_string("100")` (45 correspondances) puis la demande explicite → `AnswerTraceUiFilterPrompt`, jamais `analyze_ui_sources`.

**Le correctif** : nouveau flag `smartSearchExplicitUiSourcesQuery` (`apps/desktop/application_controller.cpp`, même liste de mots-clés que `matchUiSourcesTool`) qui **ne modifie ni `intent.kind` ni `m_pendingRecoveryAction`** — il ajoute juste une condition au bloc de traitement `if (intent.kind == AnswerTraceUiFilterPrompt ...)` pour le sauter quand la demande est explicite. Le message tombe alors jusqu'au repli générique `m_ai.processQuery()`, qui route vers `matchUiSourcesTool`. Important : `m_pendingUiStringCandidates` n'est jamais vidé dans ce cas (le bloc sauté est celui qui le vide normalement), donc `analyze_ui_sources` peut toujours s'en servir, **et** `m_pendingRecoveryAction` reste actif — une réponse simple *ultérieure* ("100" tout court) continue de router vers le pipeline `trace_ui_filter` existant, sans rien casser.

**Pourquoi ce n'est pas un risque de régression** : le guard ne matche que sur des mots-clés explicites ("analyse les sources", "sources numériques", etc.), jamais sur une simple valeur numérique — donc le flow "réponse simple = nouvelle valeur" (déjà en production, PHASE 90) n'est structurellement pas affecté.

### 2. Audit complet de la ligne "Schema obligatoire" — tranché outil par outil

| Outil | Décision PHASE 148 | Raison |
| --- | --- | --- |
| `discover_save_files` | **Ajouté au schéma** | Lecture seule, déjà dispatché + fast-path (`matchOffMemoryTool`) — visible au modèle par cohérence avec les autres outils sûrs. |
| `inspect_local_settings` | **Ajouté au schéma** | Idem. |
| `read_save_file_text` | **Ajouté au schéma** | Lecture seule (fichier déjà trouvé via `discover_save_files`), pas de fast-path dédié (le chemin exact ne s'extrait pas fiablement d'une phrase) — reste joignable via le modèle local en plusieurs tours. |
| `watch_save_file` | **Ajouté au schéma** | Attente bornée (`timeoutMs`, défaut 5s) sur un simple événement disque (`ReadDirectoryChangesW`) — pas de debugger, pas de risque de crash comparable à `find_what_writes`. |
| `kernel_write` | **Confirmé exclu, délibéré** | `risk=injection`. Sa propre description dit déjà "à utiliser seulement si l'utilisateur le demande explicitement" — reste joignable **uniquement** via fast-path déterministe sur mot-clé explicite (déjà existant, `ai/ai_engine.cpp`), jamais un choix libre du modèle. |
| `speedhack_set` | **Confirmé exclu, délibéré** | Idem — fast-path déterministe déjà existant. |
| `block_process_network` | **Confirmé exclu, délibéré + fast-path ajouté** | Même catégorie que les deux ci-dessus, mais n'avait **aucun** chemin déterministe explicite avant PHASE 148 (le même trou que les 7 originaux, trouvé en auditant). Fast-path ajouté (`"coupe le réseau"`/`"rétablis le réseau"` FR, `"block network"`/`"restore network"` EN) aux deux mêmes points que `kernel_write`/`speedhack_set`. Reste hors schéma. |
| `patch_file_bytes` | **Confirmé exclu + bug de sécurité corrigé** | **Vrai bug trouvé en auditant, pas dans le périmètre initial** : son dispatch (`apps/desktop/application_controller.cpp`) appelait `patchProcessSaveFileBytes(...)` **directement**, malgré `requiresConfirmation=true` dans le registre — un contournement RiskGate réel pour une écriture disque. Corrigé avec le même patron que `find_what_writes`/`test_candidate_fields` (PHASE 146) : redirection systématique, jamais d'exécution depuis le chat. Reste hors schéma par prudence (pas encore de fast-path explicite, chemin peu exercé). |

**Tests** : `LlamaRuntimeTest.SchemaLineListsEveryRegisteredTool` (PHASE 146) mis à jour — la liste d'exclusion ne contient plus que les 4 outils write/injection délibérément exclus. Nouveau `LlamaRuntimeTest.SchemaLineExcludesRealWriteAndInjectionTools` vérifie explicitement (assertion positive, pas juste une exclusion passive) que ces 4 restent hors schéma. Nouveaux `AIEngineContextualFallbackTest.BlockNetworkFastPathMatches*` (FR coupure + EN rétablissement).

## Inventaire complet (28 outils)

### Scan mémoire

| Outil | Risque | Confirmation | `startSmartSearch` | Pipe | Lua | Test live |
| --- | --- | --- | --- | --- | --- | --- |
| `auto_resolve` | safe | non | ✅ | ✅ | ✅ | `AGENTS.md` section "Auto-résolution IA" |
| `get_auto_report` | safe | non | ✅ (PHASE 140) | ✅ | ✅ | PHASE 140, live via pipe |
| `exact_scan` | safe | non | ✅ | ✅ | ✅ (`ke.scan_exact`) | Usage courant, voir `docs/USER_GUIDE.md` |
| `exact_scan_multi_type` | safe | non | ✅ | ✅ | ✅ | idem |
| `next_scan` | safe | non | ✅ | ✅ | ✅ (`ke.next_scan`) | idem |
| `encrypted_scan` | safe | non | ✅ | ✅ | ✅ | `AGENTS.md` section correspondante |
| `trace_ui_string` | safe | non | ✅ | ✅ | ✅ | PHASE 90 (Solitaire Bulles) |
| `analyze_ui_sources` | safe | non | ✅ (PHASE 140, limitation connue — voir note ci-dessus) | ✅ | ✅ | PHASE 140, live isolé OK ; shadowé après un vrai `trace_ui_string` |
| `read_window_text` | safe | non | ✅ | ✅ | ✅ | — |
| `start_changed_pages_diff` | safe | non | ✅ | ✅ | ✅ | — |
| `finish_changed_pages_diff` | safe | non | ✅ | ✅ | ✅ | — |
| `unknown_capture` | safe | non | ✅ | ✅ | ✅ | — |
| `unknown_compare` | safe | non | ✅ | ✅ | ✅ | — |

### Écriture / Freeze

| Outil | Risque | Confirmation | `startSmartSearch` | Pipe | Lua | Test live |
| --- | --- | --- | --- | --- | --- | --- |
| `prepare_write_checkpoint` | write | **oui** | ✅ | ✅ | ✅ | — |
| `write_value` | write | **oui** | ✅ | ✅ | ✅ | PHASE 100 smoke (écriture directe hors chat) |
| `freeze_value` | write | **oui** | ✅ | ✅ | ✅ | `AGENTS.md` "Freeze par hardware breakpoint" |

### Debug / Patch / Champ affiché-source

| Outil | Risque | Confirmation | `startSmartSearch` | Pipe | Lua | Test live |
| --- | --- | --- | --- | --- | --- | --- |
| `find_what_writes` | debug | **oui** | ✅ (PHASE 140, redirige toujours vers l'UI, n'exécute jamais) | ✅ | ✅ | `AGENTS.md` "Find What Writes" ; PHASE 140 live (confirme la redirection) |
| `analyze_field_stability` | debug | non (délibéré, PHASE 130/131 — jamais d'écriture) | ✅ | ✅ | ✅ | PHASE 131, live via pipe FR+EN |
| `generate_aob` | patch | non (reclassé PHASE 140 — vérifié lecture seule) | ✅ (PHASE 140) | ✅ | ✅ | PHASE 140, live via pipe |
| `suggest_patch` | patch | non (reclassé PHASE 140 — vérifié lecture seule) | ✅ (PHASE 140) | ✅ | ✅ | PHASE 140, live via pipe |
| `disassemble_backward` | patch | non (reclassé PHASE 140 — méthode `const`, vérifié lecture seule) | ✅ (PHASE 140) | ✅ | ✅ | PHASE 140, live via pipe |
| `test_candidate_fields` | write | **oui** | ✅ (PHASE 140, redirige toujours vers l'UI, n'exécute jamais) | ✅ | ✅ | PHASE 140 live (confirme la redirection) |

### Injection / Kernel / Système

| Outil | Risque | Confirmation | `startSmartSearch` | Pipe | Lua | Schéma modèle | Test live |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `kernel_write` | injection | **oui** | ✅ (recoveryAction cliquable) | ✅ | ✅ | ❌ exclu (PHASE 148, décision explicite — voir audit ci-dessus) | `AGENTS.md` pont kernel |
| `speedhack_set` | injection | **oui** (recoveryAction) | ✅ | ✅ | ✅ | ❌ exclu (PHASE 148, décision explicite) | — |
| `block_process_network` | injection | **oui** | ✅ | ✅ | ✅ | ❌ exclu (PHASE 148, décision explicite) — fast-path déterministe ajouté PHASE 148 (`ai/ai_engine.cpp`, `wantsNetworkOff`/`wantsNetworkOn`), voir tests `BlockNetworkFastPathMatches*` | PHASE 84, `docs/PHASE_TRACKER.md`, PHASE 148 (fast-path live via pipe) |

### Hors mémoire / UWP / fichiers de sauvegarde

| Outil | Risque | Confirmation | `startSmartSearch` | Pipe | Lua | Schéma modèle | Test live |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `discover_save_files` | safe | non | ✅ | ✅ | ✅ | ✅ ajouté PHASE 148 | PHASE 90/100, live FR+EN |
| `inspect_local_settings` | safe | non | ✅ | ✅ | ✅ | ✅ ajouté PHASE 148 | PHASE 100/121, live sur vrai package UWP |
| `read_save_file_text` | safe | non | ✅ | ✅ | ✅ | ✅ ajouté PHASE 148 | — |
| `patch_file_bytes` | write | **oui** | ✅ (bug corrigé PHASE 148 — exécutait directement malgré `requiresConfirmation=true` ; redirige maintenant sans exécuter) | ✅ | ✅ | ❌ exclu (PHASE 148, décision explicite — outil d'écriture, ne doit jamais être choisi librement par le modèle) | PHASE 148 (bug trouvé + corrigé en auditant, redirection vérifiée live) |
| `watch_save_file` | safe | non | ✅ | ✅ | ✅ | ✅ ajouté PHASE 148 | — |

### Trainer (PHASE 119/121/129)

| Outil | Risque | Confirmation | `startSmartSearch` | Pipe | Lua | Test live |
| --- | --- | --- | --- | --- | --- | --- |
| `trainer_list_features` | safe | non | ✅ | ✅ (via `callVueStoreAction`) | ✅ | PHASE 129/135, live |
| `trainer_create_write` | safe | non | ✅ | ✅ | ✅ | PHASE 129, live |
| `trainer_delete_feature` | safe | non | ✅ | ✅ | ✅ | PHASE 129, live |
| `trainer_apply_request` | write | **oui** | ✅ (prépare seulement, jamais l'action réelle) | ✅ | ✅ | PHASE 129, live |
| `trainer_restore_request` | write | **oui** | ✅ (idem) | ✅ | ✅ | PHASE 129, live |

## Notes transverses

- **Le pipe/Lua ne respecte JAMAIS `requiresConfirmation`** — c'est un bypass volontaire documenté (`AGENTS.md`, "⚠️ Bypass volontaire du RiskGate") : un appel `Q_INVOKABLE` direct via le pipe ou un script Lua async exécute l'action **immédiatement**. Seul le chemin Assistant/chat (`startSmartSearch`) respecte `requiresConfirmation` en renvoyant `requires_confirmation`/une `recoveryAction` cliquable plutôt que d'agir. Un futur PHASE 120 doit raisonner sur CE chemin-ci (Assistant), pas supposer que le comportement pipe s'applique.
- **`trainer_apply_request`/`trainer_restore_request`** ne déclenchent jamais `applyTrainerFeature`/`restoreTrainerFeature` directement même via `startSmartSearch` — ils renvoient un message invitant à confirmer dans l'onglet Trainer (piège PHASE 121 : ces actions ouvrent un vrai modal `confirmRiskAction`, qui timeout ou bloque si appelé en autonome sans humain présent).
- **Outils à risque reclassés en lecture seule** : `analyze_field_stability` reste `risk=debug` mais `requiresConfirmation=false` (PHASE 131), et `generate_aob`/`suggest_patch`/`disassemble_backward` restent `risk=patch` mais `requiresConfirmation=false` (PHASE 146/ancien libellé interne PHASE 140). Décision délibérée dans les deux cas : ces chemins lisent/analysent/suggèrent, mais n'écrivent jamais. `find_what_writes` et `test_candidate_fields` restent confirmés/redirigés UI, car ils attachent un debugger ou peuvent écrire une valeur test.
- **Colonne "Test live" incomplète par construction** : ce document recense ce qui est *connu et documenté ailleurs* au 26/08/2026, pas un audit exhaustif de chaque outil relancé pour l'occasion — un `—` invite à vérifier avant de s'appuyer dessus dans un contexte sensible (cible réelle non-test), pas à conclure que l'outil est cassé.

## Liens

- `docs/PHASE_TRACKER.md` — historique détaillé de chaque phase citée ici.
- `docs/POWER_UP_ROADMAP.md` — priorisation produit, section "État courant".
- `AGENTS.md` — détail par module (comment ça marche, pièges connus, tests ciblés).
- `docs/KILLENGINE_TOOLS_AND_CAPABILITIES.md` — inventaire produit/utilisateur (vues, workflows), pas la surface Assistant.
- `ai/tool_registry.cpp` — source de vérité pour risque/confirmation/requiredArgs (relire avant de faire confiance à ce document si le registry a pu changer depuis).
