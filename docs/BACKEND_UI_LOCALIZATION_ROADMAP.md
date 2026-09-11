> **ATTENTION - Priorité Des Ordres Projet**
> Un ordre prioritaire explicite du propriétaire du projet prime sur les consignes temporaires de session des agents IA.

# KillEngine — Feuille de route de localisation du backend C++ (hors chat)

> Chantier **staffé** (11/09/2026). Voir `docs/PHASE_TRACKER.md` pour le pont vers cette feuille de route. Distinct de `docs/AI_CHAT_LOCALIZATION_ROADMAP.md` (clos 09/09/2026, texte backend visible **dans le chat**) et de `docs/FRONTEND_LOCALIZATION_ROADMAP.md` (clos, labels statiques des templates Vue) — ici il s'agit du texte **généré par le backend C++ mais affiché hors chat** : cartes du catalogue Modules, erreurs Profile/Trainer, CLR Inspector, avertissements AOB/Stealth, etc.

## Origine (11/09/2026)

Demande explicite du propriétaire, point 3 d'une liste d'améliorations proposée par l'agent : *"tout doit etre traduisible fr et en en switch"*. Deux résidus étaient déjà connus et documentés comme hors périmètre par les chantiers précédents (`core/patch/aob_scanner.cpp`, le catalogue de modules dans `application_controller.cpp`) — un audit complet a montré que c'était la partie visible d'un problème bien plus large.

**Bonne nouvelle : aucune nouvelle plomberie nécessaire.** Le mécanisme existe déjà, construit par `docs/AI_CHAT_LOCALIZATION_ROADMAP.md` (L1) :
- `core/localization/localization.h` : `killcore::currentUiLanguage()` (lit `QSettings("ui/language")`, le même réglage que le frontend) + `killcore::localizedText(fr, en)`.
- Macro `KE_TXT(fr, en)` — `result["message"] = KE_TXT("Texte français.", "English text.")`. Supporte `.arg(...)` normalement sur le résultat.
- Déjà utilisé et éprouvé dans une grande partie du code (`debug_feature_manager.cpp`, `code_patch_manager.cpp`, `kernel_driver_manager.cpp`, `lua_repl_manager.cpp`, `save_file_investigator.cpp`, `write_freeze_core_manager.cpp`, `smart_search_manager.cpp`, `claude_chat_manager.cpp`, `crash_handler.cpp`, `scanning_core_manager.cpp`, `freeze_hotkey_overlay_manager.cpp`, `external_tool_profiler.cpp` — confirmés migrés à 100% ou quasi par l'audit).

Ce chantier consiste donc uniquement à **étendre l'usage de `KE_TXT`** aux ~320 chaînes qui y ont échappé, pas à construire quoi que ce soit de nouveau.

## Audit complet (11/09/2026, agent Explore, lecture seule)

Recherche systématique sur `apps/desktop/*.cpp` (28 fichiers), `core/**/*.cpp` (75 fichiers), `ai/*.cpp` (15 fichiers) — hors `build/`, `tests/`, tiers. Méthode : grep accents français + vocabulaire français sans accent (leçon du chantier frontend : un grep accents seul en rate) dans les contextes d'affectation (`result["x"] =`, `item["x"] =`, `.error =`, `.message =`, `.warning =`, `return "..."`, `emit signal(...)`), en excluant les `KE_LOG_*` (logs debug, jamais montrés à l'utilisateur), les commentaires, et les chaînes déjà dans `KE_TXT(...)`.

**Total : ~320 chaînes individuelles sur ~30 fichiers**, dont la quasi-totalité (~310) à **faible risque** (texte d'affichage pur, aucun changement de logique).

**Explicitement hors périmètre** (confirmé par l'audit, ne pas traiter dans ce chantier) : `ai/tool_registry.cpp`, le texte de construction du prompt dans `ai/llama_runtime.cpp::buildPrompt`, et les listes de mots-clés `q.contains("...")` dans `ai/ai_engine.cpp` — ce sont des descriptions de schéma envoyées au LLM ou des mots-clés matchés contre le texte de l'utilisateur, jamais du texte affiché à l'utilisateur.

### ⚠️ Piège de couplage trouvé — à corriger dans le même round que la source

`ai/auto_resolver.cpp:166` fait du pattern-matching sur le **texte français** de 4 messages d'erreur AOB/Trainer :
```cpp
if (error.contains("bloqu") || error.contains("trop faible") || error.contains("non unique"))
```
Ces 4 messages viennent de `apps/desktop/profile_manager.cpp:975,992,1145,1172` (télémétrie `trainer_patch_save_blocked`/`trainer_patch_apply_blocked`). Si ces messages sont traduits sans toucher ce détecteur, la télémétrie s'arrête silencieusement de fonctionner pour un utilisateur en anglais. **Fix obligatoire dans le même round que B3** (voir Décisions de méthode) : ajouter un champ `errorCode` structuré (ex. `"aob_weak"`, `"aob_not_unique"`) que `auto_resolver.cpp` checke à la place du texte affiché — même pattern que `trainerDependencies.ts`/`resolveOrderErrorMessage()` déjà établi côté frontend (chantier de localisation frontend, round U27).

## Décisions de méthode

1. **Mécanisme** : `KE_TXT(fr, en)`, déjà en place, aucune nouvelle plomberie. Pas de nouveau système à inventer.
2. **Couplage texte affiché / logique interne** (même leçon que le chantier frontend, où ce bug est apparu 3 fois) : avant de traduire une chaîne, vérifier qu'elle n'est pas aussi lue ailleurs pour une comparaison/un test (`contains`, `==`, parsing). Le seul cas trouvé par l'audit est le couplage `auto_resolver.cpp` ci-dessus, mais rester vigilant en migrant B3-B10 au cas où l'audit en aurait raté un.
3. **Balayage en deux passes obligatoire par fichier** (leçon déjà appliquée cette session) : grep accents (`Grep` tool, jamais `Bash grep` — corrompt les résultats sur cet environnement) **ET** grep mots français courants sans accent, avant de considérer un fichier terminé. L'audit initial a lui-même noté des chaînes sans accent qui seraient invisibles à un grep accents seul (ex. `clr_inspector_bridge.cpp`).
4. **Vérification** : `scripts/build.ps1` + `killengine_unit_tests.exe` (469/469 attendu, +quelques nouveaux tests pour le fix `errorCode` de B3) après chaque round. Contrairement au chantier frontend, la plupart de ces chemins ne sont pas vérifiables visuellement en un clic — vérification par relecture de diff + build propre pour la majorité, complétée par une vérification live CDP réelle **où c'est raisonnablement faisable** (B1 : page Modules, déjà fait aujourd'hui pour `edr_exclusion` ; B2 : scan AOB réel, déjà fait aujourd'hui).
5. **Frontend override à retirer une fois B1 fait** : `ModulesView.vue`'s `moduleName()`/`moduleDescription()`/`moduleDetail()` (ajoutées le 10/09 pour contourner le texte brut d'`edr_exclusion` sans toucher au C++) deviennent redondantes une fois B1 clos — le backend renverra directement le bon texte selon la langue. À retirer dans le même round que B1 pour ne pas garder deux sources de vérité sur le même texte.
6. **Ne pas toucher** : `ai/tool_registry.cpp`, le prompt `ai/llama_runtime.cpp::buildPrompt`, les listes de mots-clés `ai/ai_engine.cpp` (confirmé hors périmètre par l'audit — schéma LLM / matching sur l'entrée utilisateur, jamais affiché).

## Candidats (priorisés par visibilité réelle, pas par ordre de fichier)

| # | Candidat | Fichier(s) | Chaînes (audit) | Priorité | Statut |
| --- | --- | --- | --- | --- | --- |
| [x] B1 | Catalogue Modules (8 cartes) + flux install/téléchargement/UAC/exclusion | `apps/desktop/application_controller.cpp` (`getModuleCatalog()`, `installModule()`, `checkEdrBlocking()`, `addEdrExclusionAsync()`, `checkDebugPrivilege()`/`enableDebugPrivilege()`), + résidus visibles trouvés en vérification live : `apps/desktop/settings_diagnostics_manager.cpp::getAiModelStatus()`, `core/kernel/kernel_driver_bridge.cpp::probe()` | ~58 chaînes traitées | **Haute** | **Fait 11/09/2026** |
| [x] B2 | Avertissements de qualité de signature AOB | `core/patch/aob_scanner.cpp` | 8 | **Haute** | **Fait 11/09/2026** |
| [x] B3 | Profile/Trainer (sauvegarde/application de patch) + Auto Resolver (labels d'étapes + insights) + **fix du couplage `errorCode`** | `apps/desktop/profile_manager.cpp`, `ai/auto_resolver.cpp` | ~85 | **Haute-moyenne** | **Fait 11/09/2026** |
| [x] B4 | CLR Inspector (erreurs de résolution locator, setter, écriture struct) | `apps/desktop/clr_inspector_bridge.cpp` | ~26 | Moyenne | **Fait 11/09/2026** |
| [x] B5 | Cluster stealth/injection (pattern répétitif "CreateFileMapping/MapViewOfFile/déjà actif") | `core/debug/stealth_profiler.cpp`, `core/debug/inprocess_breakpoint.cpp`, `core/debug/page_guard.cpp`, `core/inject/dll_injector.cpp`, `core/inject/lag_switch.cpp`, `core/inject/http_proxy.cpp`, `core/inject/api_hook.cpp`, `core/debug/speedhack.cpp` | ~85 | Basse-moyenne | **Fait 11/09/2026** |
| [x] B6 | Driver kernel + DPAPI + relais patch PowerShell | `core/kernel/kernel_driver_bridge.cpp` (déjà fait pendant B1), `core/security/dpapi_key_store.cpp`, `core/patch/code_patch.cpp`, `core/patch/instruction_patch_suggester.cpp` | ~30 | Basse-moyenne | **Fait 11/09/2026** |
| [ ] B7 | Scanner core (auto-dissect, classification source d'affichage, parsing valeur, résolution export) | `core/scanner/auto_dissect.cpp`, `core/scanner/display_source_classifier.cpp`, `core/scanner/scan_types.cpp` (⚠️ ~18 appelants, vérifier chaque site après correction), `core/process/export_resolver.cpp` | ~25 | Basse-moyenne | Pas commencé |
| [ ] B8 | Investigation UWP/save-file + surveillance fichier | `core/process/package_storage.cpp`, `core/process/file_watch.cpp` | ~15 | Basse — fonctionnalité de niche | Pas commencé |
| [ ] B9 | Auto-assembler + client CDP WebView2 | `core/scripting/auto_assembler.cpp`, `core/webview2/cdp_client.cpp` | ~6 | Basse | Pas commencé |
| [ ] B10 | Réglages/diagnostics + petits fichiers résiduels | `apps/desktop/settings_diagnostics_manager.cpp`, `apps/desktop/memory_timeline_manager.cpp`, `apps/desktop/debug_feature_manager.cpp` (1 ligne oubliée), `apps/desktop/automation_pipe_manager.cpp`, `apps/desktop/automation_pipe_server.cpp`, `apps/desktop/investigation_notebook_manager.cpp`, `apps/desktop/memory_heatmap_manager.cpp` | ~25 | Basse | Pas commencé |

## Ordre recommandé

1. **B1, B2** — déjà vus en direct aujourd'hui, gain de visibilité immédiat, risque quasi nul.
2. **B3** — inclut le fix de couplage obligatoire (`errorCode`), à faire tôt pour ne pas laisser la télémétrie Auto Resolver cassée entre deux sessions si quelqu'un d'autre touche `profile_manager.cpp` entre-temps.
3. **B4-B7** — reste des panneaux d'usage réel (CLR Inspector, stealth, kernel/DPAPI, scanner core), dans n'importe quel ordre selon disponibilité agent.
4. **B8-B10** — résidus de fonctionnalités de niche ou petits fichiers, faible priorité, chantier permanent comme `docs/REFACTOR_ROADMAP.md`.

## Progrès

### 11/09/2026 — B1 clos + bug de fond découvert et corrigé

B1 traité (catalogue Modules + flux install/UAC/EDR/debug privilege), ~58 chaînes migrées vers `KE_TXT`. L'override frontend `ModulesView.vue` (`moduleName`/`moduleDescription`/`moduleDetail`, ajouté le 10/09 pour contourner `edr_exclusion` sans toucher au C++) a été retiré — le backend renvoie directement le bon texte, les 3 clés i18n `modules.catalog.edrExclusion.*` correspondantes ont été supprimées des deux locales.

**Découverte critique en vérification live (CDP, méthode de ce chantier — voir Décisions de méthode §4)** : le switch rapide FR/EN de la sidebar (`App.vue`, ajouté le 10/09) et les boutons FR/EN de la page Réglages ne persistaient JAMAIS `QSettings("ui/language")` — seul le bouton "Sauvegarder" complet de Réglages le faisait. Résultat : tout texte backend `KE_TXT` (déjà utilisé par le chat IA depuis le chantier clos `AI_CHAT_LOCALIZATION_ROADMAP.md`, et maintenant par B1) restait bloqué sur la dernière langue **sauvegardée**, indépendamment de la langue affichée à l'écran — un switch qui n'en était qu'à moitié un. Corrigé par une nouvelle méthode légère `ApplicationController::setUiLanguage(language)` (→ `SettingsDiagnosticsManager::setUiLanguage`, écrit uniquement `ui/language` sans toucher aux autres réglages) appelée immédiatement par le nouveau store action `switchLanguage()` (`ui/src/stores/settings.ts`), câblée à la fois sur le switch sidebar et sur les boutons de Réglages. Vérifié live : catalogue Modules bascule réellement FR→EN après ce fix (avant : restait figé en français malgré le switch).

Cette découverte a aussi révélé une fragilité de test latente : 8 tests de `tests/unit/test_ai_tools.cpp` (`AIEngineContextualFallbackTest`) asserient du texte français en dur sans isoler `ui/language`, et ont commencé à échouer dès que le fix ci-dessus a réellement persisté "en" sur le registre de la machine de dev pendant la vérification live. Corrigé en ajoutant un garde `ScopedUiLanguage` (même patron que `test_localization.cpp`) sur les 8 tests concernés — suite verte confirmée avec `ui/language` forcé à "en" pendant l'exécution.

Build C++ propre, 469/469 tests unitaires (y compris avec langue ambiante forcée en "en"), `npm run type-check`/`npm run build` propres, vérification visuelle CDP FR et EN sur la page Modules réelle.

### 11/09/2026 — B6 clos (driver kernel + DPAPI + patch)

`core/kernel/kernel_driver_bridge.cpp` était déjà entièrement migré pendant B1 (aucun résidu). `dpapi_key_store.cpp` (5 chaînes, chiffrement clé API Claude), `code_patch.cpp` (relais PowerShell de patch, fallback ACCESS_DENIED), `instruction_patch_suggester.cpp` (labels/descriptions des suggestions de patch NOP/branch/compare/call/INT3/RET) migrés vers `KE_TXT`. Aucun couplage trouvé (categorie/riskLevel restent des codes stables séparés du label affiché).

**Test de régression trouvé** : 3 tests de `tests/unit/test_aob_scanner.cpp` (`InstructionPatchSuggester.Suggests*`) asserient les labels français en dur — même fragilité que B1/B3, corrigée avec le même garde `ScopedUiLanguage`. Build + 470/470 tests propres (y compris langue ambiante forcée en "en").

### 11/09/2026 — B5 clos (cluster stealth/injection)

8 fichiers migrés vers `KE_TXT` : `stealth_profiler.cpp` (analyse de menaces/recommandations), `inprocess_breakpoint.cpp`, `page_guard.cpp`, `dll_injector.cpp`, `lag_switch.cpp`, `http_proxy.cpp`, `api_hook.cpp`, `speedhack.cpp` (tous partagent le même patron IPC : CreateFileMapping/MapViewOfFile/injection/« déjà actif »). Plusieurs chaînes étaient déjà en anglais uniquement (jamais traduites) — bilinguisées à cette occasion pour une couverture complète.

**Deuxième bug de couplage trouvé et corrigé** (même classe que B3) : `dll_injector.cpp::injectDll()` comparait `result.error == QStringLiteral("LoadLibraryW returned NULL in remote process")` pour décider de retenter l'injection via une copie AppContainer — une comparaison sur le texte affiché, qui aurait cassé silencieusement dès que ce texte est traduit (et qui a d'ailleurs immédiatement cassé même en anglais, le nouveau texte KE_TXT différant légèrement du littéral comparé). Corrigé avec un indicateur booléen local `loadLibraryReturnedNull`, réinitialisé à chaque appel de `loadDllPath`, indépendant du texte affiché.

Build + 470/470 tests propres (y compris langue ambiante forcée en "en"). Aucun couplage frontend détecté (`SpeedhackView.vue` et les autres vues concernées n'inspectent jamais le texte d'erreur).

### 11/09/2026 — B4 clos

`apps/desktop/clr_inspector_bridge.cpp` (26 chaînes résiduelles — le fichier était déjà partiellement localisé) migré vers `KE_TXT` : résolution de setter d'instance, encodage de paramètres struct, écriture struct, décodage de code natif JITté. Confirmé sans couplage frontend (les seuls `.includes(...)` sur ce domaine dans `ClrInspectorView.vue` portent sur des noms de type CLR techniques, jamais traduits). Build + 470/470 tests propres (y compris langue ambiante forcée en "en").

### 11/09/2026 — B3 clos (Profile/Trainer + Auto Resolver + fix de couplage)

`apps/desktop/profile_manager.cpp` (~65 chaînes) et `ai/auto_resolver.cpp` (~20 chaînes : `stepTypeToString`, les 10 étapes de `planForGoal`, les 7 insights de télémétrie avec label/reason/nextAction, `displayValuePattern`/`displayValueRecommendation`) migrés vers `KE_TXT`.

**Fix de couplage appliqué** (le risque documenté dès la création de cette roadmap) : `ai/auto_resolver.cpp:166` faisait du pattern-matching sur le texte affiché français (`error.contains("bloqu")`/`"trop faible"`/`"non unique"`) pour incrémenter `report.trainerBlockedCount`. Ajout d'un champ `result["errorCode"]` stable (`aob_signature_too_weak`, `aob_signature_not_unique`, `aob_signature_not_found`, `aob_signature_not_found_version_mismatch`) aux 4 sites de blocage de `profile_manager.cpp` (sauvegarde ET application de patch Trainer) ; `auto_resolver.cpp` checke désormais `errorCode` au lieu du texte.

**Vérification live end-to-end via l'automation pipe** (`KillEngineTestTarget.exe`, `saveProfileCodePatch` avec une signature AOB volontairement faible) : en français, `errorCode: "aob_signature_too_weak"` + message FR corrects, `trainerBlockedCount` passe à 1. Après `setUiLanguage("en")`, même appel avec une autre adresse : message intégralement en anglais, `errorCode` inchangé, `trainerBlockedCount` passe à 2 — preuve directe que l'ancien bug (comptage cassé en anglais) est résolu et que le nouveau mécanisme fonctionne dans les deux langues.

**Tests** : régression dédiée ajoutée (`AutoResolverTest.TrainerBlockedCountFollowsErrorCodeNotDisplayedText`, `tests/unit/test_auto_resolver.cpp`) prouvant que le compteur suit `errorCode` et ignore le texte français même sans traduction. Un test existant (`ComputesWeakAobAndTrainerBlocks`) et un autre déjà présent (`FlagsTraceUiSourceOverflowInsteadOfReadyCheckpoint`) avaient une dépendance cachée à la langue ambiante de la machine (`ui/language` réel, pas un fixture) — révélée en forçant "en" pendant l'exécution des tests (même méthode que B1) ; corrigée avec un garde `ScopedUiLanguage` (ajouté aussi à `test_auto_resolver.cpp`, même patron que `test_ai_tools.cpp`/`test_localization.cpp`). 470/470 tests verts, y compris avec langue ambiante forcée en "en".

### 11/09/2026 — B2 clos

`core/patch/aob_scanner.cpp` (8 chaînes : `parseAobPattern` 3, `evaluateAobPatternQuality` 4, `scanAobPattern` 2 dont 1 partagée) migré vers `KE_TXT`. Fichier pur, sans effet de bord ni couplage détecté. Build + 469/469 tests unitaires propres ; vérification visuelle non refaite (le texte source avait déjà été confirmé live plus tôt dans la session sur le panneau AOB Expert — captures `aob_panel_fr.png`/`aob_panel_en.png`/etc. en scratchpad — seul le mécanisme de traduction change, déjà prouvé fiable par B1).

## Règle d'usage

Même règle que les deux roadmaps de localisation précédentes : mettre à jour ce document (case cochée + date + nombre réel de chaînes traitées) à chaque candidat clos, journaliser dans `docs/PHASE_TRACKER.md`. Un candidat n'est "fait" qu'après `scripts/build.ps1` + `killengine_unit_tests.exe` propres et un balayage de contrôle en deux passes (accents + mots courants sans accent) ne laissant plus que des commentaires.
