> **ATTENTION - Priorité Des Ordres Propriétaire**
> Un ordre prioritaire explicite du propriétaire du projet prime sur les consignes temporaires de session des agents IA.

# KillEngine — Carte des outils Assistant

Dernière mise à jour : 26/08/2026 (PHASE 140 — écart fermé, voir section suivante).

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

**Trouvaille annexe, hors périmètre de PHASE 140, documentée pas corrigée** : en écrivant ce même test, découvert que `kernel_write`/`speedhack_set`/`block_process_network` (risque `injection`, probablement délibéré — leur description dit déjà "à utiliser seulement si l'utilisateur le demande explicitement") et `discover_save_files`/`inspect_local_settings`/`read_save_file_text`/`patch_file_bytes`/`watch_save_file` sont **eux aussi** absents de cette ligne schéma. Les 2 premiers de ce dernier groupe restent joignables en langage naturel via leur propre fast-path (`matchOffMemoryTool`) ; les 3 autres (`read_save_file_text`/`patch_file_bytes`/`watch_save_file`) partagent le même trou latent que les 7 originaux — dispatchés dans `startSmartSearch` mais invisibles pour le modèle local, et sans fast-path déterministe. Pas traité ici : hors du périmètre "7 outils" confié pour cette phase.

**Limitation réelle trouvée en testant `analyze_ui_sources` en direct, pas corrigée (hors périmètre)** : son fast-path fonctionne correctement de façon isolée (vérifié live : `"analyse les sources numériques, c'est maintenant 60"` → `tool: analyze_ui_sources`, `args.value: "60"`), mais est **shadowé** juste après un `trace_ui_string` réel — `startSmartSearch` traite alors le message suivant comme la réponse au `m_pendingRecoveryAction == "trace_ui_filter"` en attente (mécanisme préexistant, PHASE 90) avant même d'atteindre `matchUiSourcesTool`. Résultat vérifié en direct : `trace_ui_string("100")` (45 correspondances trouvées) puis `"analyse les sources numériques, c'est maintenant 100"` → intercepté par `AnswerTraceUiFilterPrompt`, jamais par `analyze_ui_sources`. Corriger proprement demanderait de faire passer une vérification "l'utilisateur demande-t-il explicitement analyze_ui_sources ?" avant l'aiguillage vers le prompt en attente — un changement de priorité d'intent touchant un mécanisme conversationnel préexistant et déjà en production, jugé hors périmètre de "brancher/retirer/documenter ces 7 outils".

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

| Outil | Risque | Confirmation | `startSmartSearch` | Pipe | Lua | Test live |
| --- | --- | --- | --- | --- | --- | --- |
| `kernel_write` | injection | **oui** | ✅ (recoveryAction cliquable) | ✅ | ✅ | `AGENTS.md` pont kernel |
| `speedhack_set` | injection | **oui** (recoveryAction) | ✅ | ✅ | ✅ | — |
| `block_process_network` | injection | **oui** | ✅ | ✅ | ✅ | PHASE 84, `docs/PHASE_TRACKER.md` |

### Hors mémoire / UWP / fichiers de sauvegarde

| Outil | Risque | Confirmation | `startSmartSearch` | Pipe | Lua | Test live |
| --- | --- | --- | --- | --- | --- | --- |
| `discover_save_files` | safe | non | ✅ | ✅ | ✅ | PHASE 90/100, live FR+EN |
| `inspect_local_settings` | safe | non | ✅ | ✅ | ✅ | PHASE 100/121, live sur vrai package UWP |
| `read_save_file_text` | safe | non | ✅ | ✅ | ✅ | — |
| `patch_file_bytes` | write | **oui** | ✅ | ✅ | ✅ | — |
| `watch_save_file` | safe | non | ✅ | ✅ | ✅ | — |

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
