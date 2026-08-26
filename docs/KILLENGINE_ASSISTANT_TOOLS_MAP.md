> **ATTENTION - Priorité Des Ordres Propriétaire**
> Un ordre prioritaire explicite du propriétaire du projet prime sur les consignes temporaires de session des agents IA.

# KillEngine — Carte des outils Assistant

Dernière mise à jour : 26/08/2026 (PHASE 139).

**Pourquoi ce document existe** : préparation explicite de PHASE 120 (Assistant mode réflexion/enquête, volontairement repoussée en fin d'arsenal — voir `docs/PHASE_TRACKER.md`) sans coder PHASE 120 elle-même. `docs/POWER_UP_ROADMAP.md` dit noir sur blanc que ce futur mode "doit synthétiser tous les outils stabilisés... avec leurs usages, risques, limites" — cet inventaire structuré est ce socle, pas le mode lui-même.

**Ce que ce document n'est pas** : un duplicata de `docs/KILLENGINE_TOOLS_AND_CAPABILITIES.md` (inventaire produit/utilisateur, prose par vue) ni de `AGENTS.md` (contexte architectural détaillé par module, avec pièges historiques). Ce document est spécifiquement la surface **`ai/tool_registry.cpp`** — les 28 outils que l'Assistant (fast-path déterministe ou modèle local llama.cpp) peut choisir d'appeler — croisée avec leur chemin d'exécution réel. Pour le détail d'un module (comment il marche, ses pièges), suivre le lien vers `AGENTS.md`/`docs/PHASE_TRACKER.md` indiqué en note.

## Légende des colonnes

- **Risque** : catégorie `risk` dans `ai/tool_registry.cpp` (`safe`/`write`/`debug`/`patch`/`injection`).
- **Confirmation** : `requiresConfirmation` — si `true`, un vrai `confirmRiskAction` (modal RiskGate) doit être cliqué côté UI ; l'Assistant ne peut jamais l'auto-approuver (voir `AGENTS.md` section "Connecteur d'automatisation locale" sur le bypass volontaire du RiskGate **par le pipe**, qui ne s'applique PAS à ce chemin Assistant/chat).
- **Dispatché par `startSmartSearch`** : le vrai dispatcher C++ qui exécute un tool_call choisi par l'Assistant (`apps/desktop/application_controller.cpp::startSmartSearch`, **pas** une fonction nommée `executeToolCall` malgré ce qu'on pourrait supposer). `❌` = choisi par le modèle, ce tool produirait `actionStatus: "unsupported_tool"` — enregistré/annoncé au LLM mais jamais réellement branché. Vérifié par grep systématique le 26/08/2026 (PHASE 139), pas supposé.
- **Pipe / Lua** : d'après la convention PHASE 127 (`AGENTS.md`), **toute** méthode `Q_INVOKABLE` d'`ApplicationController` est automatiquement joignable via le pipe d'automatisation (réflexion `QMetaMethod`) et donc via `ke.call(...)` en Lua — colonnes donc `✅` par défaut dès qu'une méthode `Q_INVOKABLE` sous-jacente existe, indépendamment du dispatch `startSmartSearch`. `➖` si aucune méthode `Q_INVOKABLE` correspondante n'a été identifiée (le tool n'existe alors que comme concept enregistré, sans backend direct connu).
- **Test live** : référence courte vers où trouver une preuve de fonctionnement en conditions réelles (pas juste compilé). `—` = pas de preuve live connue à la date de ce document (ne veut pas dire "cassé", veut dire "à vérifier avant de s'appuyer dessus dans un contexte sensible").

## Constat principal (à retenir avant tout le reste)

**7 des 28 outils enregistrés dans `ai/tool_registry.cpp` — donc annoncés au modèle local comme choix valides — ne sont PAS dispatchés par `startSmartSearch`.** Si le modèle (ou un fast-path futur mal câblé) les choisit, la réponse est `actionStatus: "unsupported_tool"`, `actionError: "Outil Smart Search non supporté: <nom>"` — un échec propre, pas un crash, mais une frustration utilisateur évitable puisque la fonctionnalité sous-jacente existe et marche (accessible par bouton UI direct, par pipe, ou par Lua). Tous les 7 ont une méthode `Q_INVOKABLE` réelle et fonctionnelle en arrière-plan :

| Outil | Méthode `Q_INVOKABLE` réelle | Accessible aujourd'hui via |
| --- | --- | --- |
| `get_auto_report` | `getAutoResolveReport(maxEvents)` | Pipe, Lua, panneau Assistant (rapport auto-résolution) |
| `analyze_ui_sources` | `analyzeUiStringSources(...)` | Pipe, Lua, bouton "Analyser sources" (Trace UI string) |
| `find_what_writes` | `findWhatWrites(addressHex, options)` | Pipe, Lua, bouton "Écrit par" (ExpertView) |
| `generate_aob` | `generateAobSignature(addressHex, options)` | Pipe, Lua, panneau AOB/Patch |
| `suggest_patch` | `suggestCodePatches(addressHex, options)` | Pipe, Lua, panneau AOB/Patch |
| `disassemble_backward` | `disassembleBackward(addressHex, options)` | Pipe, Lua, workflow champ affiché/source (manuel) |
| `test_candidate_fields` | `testCandidateFieldsAsync(...)` | Pipe, Lua, workflow champ affiché/source (manuel) |

**À faire si repris un jour** (hors périmètre de ce document, volontairement pas codé ici) : soit ajouter les 7 `else if (tool == "...")` manquants dans `startSmartSearch` (patron déjà établi par PHASE 100/129/131 — `matchXxxTool` dans `ai/ai_engine.cpp` + case de dispatch dans `application_controller.cpp`), soit retirer ces 7 entrées de `ai/tool_registry.cpp` si on décide qu'elles ne doivent jamais être choisies en langage naturel (rester des actions UI/pipe/Lua pures). Décision produit à trancher, pas tranchée par ce document.

## Inventaire complet (28 outils)

### Scan mémoire

| Outil | Risque | Confirmation | `startSmartSearch` | Pipe | Lua | Test live |
| --- | --- | --- | --- | --- | --- | --- |
| `auto_resolve` | safe | non | ✅ | ✅ | ✅ | `AGENTS.md` section "Auto-résolution IA" |
| `get_auto_report` | safe | non | ❌ | ✅ | ✅ | — |
| `exact_scan` | safe | non | ✅ | ✅ | ✅ (`ke.scan_exact`) | Usage courant, voir `docs/USER_GUIDE.md` |
| `exact_scan_multi_type` | safe | non | ✅ | ✅ | ✅ | idem |
| `next_scan` | safe | non | ✅ | ✅ | ✅ (`ke.next_scan`) | idem |
| `encrypted_scan` | safe | non | ✅ | ✅ | ✅ | `AGENTS.md` section correspondante |
| `trace_ui_string` | safe | non | ✅ | ✅ | ✅ | PHASE 90 (Solitaire Bulles) |
| `analyze_ui_sources` | safe | non | ❌ | ✅ | ✅ | — |
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
| `find_what_writes` | debug | **oui** | ❌ | ✅ | ✅ | `AGENTS.md` "Find What Writes", nombreux tests d'intégration |
| `analyze_field_stability` | debug | non (délibéré, PHASE 130/131 — jamais d'écriture) | ✅ | ✅ | ✅ | PHASE 131, live via pipe FR+EN |
| `generate_aob` | patch | **oui** | ❌ | ✅ | ✅ | — |
| `suggest_patch` | patch | **oui** | ❌ | ✅ | ✅ | — |
| `disassemble_backward` | patch | **oui** | ❌ | ✅ | ✅ | — |
| `test_candidate_fields` | write | **oui** | ❌ | ✅ | ✅ | — |

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
- **`analyze_field_stability`** est le seul outil `risk=debug` avec `requiresConfirmation=false` — choix délibéré (PHASE 131) car il n'écrit jamais, contrairement à `find_what_writes`/`generate_aob`/`suggest_patch`/`disassemble_backward` qui exigent tous une confirmation malgré des risques différents (attache un debugger vs modifie potentiellement du code).
- **Colonne "Test live" incomplète par construction** : ce document recense ce qui est *connu et documenté ailleurs* au 26/08/2026, pas un audit exhaustif de chaque outil relancé pour l'occasion — un `—` invite à vérifier avant de s'appuyer dessus dans un contexte sensible (cible réelle non-test), pas à conclure que l'outil est cassé.

## Liens

- `docs/PHASE_TRACKER.md` — historique détaillé de chaque phase citée ici.
- `docs/POWER_UP_ROADMAP.md` — priorisation produit, section "État courant".
- `AGENTS.md` — détail par module (comment ça marche, pièges connus, tests ciblés).
- `docs/KILLENGINE_TOOLS_AND_CAPABILITIES.md` — inventaire produit/utilisateur (vues, workflows), pas la surface Assistant.
- `ai/tool_registry.cpp` — source de vérité pour risque/confirmation/requiredArgs (relire avant de faire confiance à ce document si le registry a pu changer depuis).
