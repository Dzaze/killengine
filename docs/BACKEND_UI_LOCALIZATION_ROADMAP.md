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
| [ ] B1 | Catalogue Modules (8 cartes) + flux install/téléchargement/UAC/exclusion | `apps/desktop/application_controller.cpp` (fonction `getModuleCatalog()` et alentours, ~lignes 5760-6650) | ~45 | **Haute** — page Modules, déjà partiellement vue en direct aujourd'hui (`edr_exclusion`) | Pas commencé |
| [ ] B2 | Avertissements de qualité de signature AOB | `core/patch/aob_scanner.cpp` | 5 | **Haute** — déjà observé en direct aujourd'hui (panneau AOB Expert) | Pas commencé |
| [ ] B3 | Profile/Trainer (sauvegarde/application de patch) + Auto Resolver (labels d'étapes + insights) + **fix du couplage `errorCode`** | `apps/desktop/profile_manager.cpp`, `ai/auto_resolver.cpp` | ~110 | **Haute-moyenne** — panneaux Profile/Trainer d'usage courant ; inclut le fix de couplage obligatoire | Pas commencé |
| [ ] B4 | CLR Inspector (erreurs de résolution locator, setter, écriture struct) | `apps/desktop/clr_inspector_bridge.cpp` | ~50 | Moyenne | Pas commencé |
| [ ] B5 | Cluster stealth/injection (pattern répétitif "CreateFileMapping/MapViewOfFile/déjà actif") | `core/debug/stealth_profiler.cpp`, `core/debug/inprocess_breakpoint.cpp`, `core/debug/page_guard.cpp`, `core/inject/dll_injector.cpp`, `core/inject/lag_switch.cpp`, `core/inject/http_proxy.cpp`, `core/inject/api_hook.cpp`, `core/debug/speedhack.cpp` | ~90 | Basse-moyenne — très répétitif, traitement mécanique rapide malgré le volume | Pas commencé |
| [ ] B6 | Driver kernel + DPAPI + relais patch PowerShell | `core/kernel/kernel_driver_bridge.cpp`, `core/security/dpapi_key_store.cpp`, `core/patch/code_patch.cpp`, `core/patch/instruction_patch_suggester.cpp` | ~25 | Basse-moyenne | Pas commencé |
| [ ] B7 | Scanner core (auto-dissect, classification source d'affichage, parsing valeur, résolution export) | `core/scanner/auto_dissect.cpp`, `core/scanner/display_source_classifier.cpp`, `core/scanner/scan_types.cpp` (⚠️ ~18 appelants, vérifier chaque site après correction), `core/process/export_resolver.cpp` | ~25 | Basse-moyenne | Pas commencé |
| [ ] B8 | Investigation UWP/save-file + surveillance fichier | `core/process/package_storage.cpp`, `core/process/file_watch.cpp` | ~15 | Basse — fonctionnalité de niche | Pas commencé |
| [ ] B9 | Auto-assembler + client CDP WebView2 | `core/scripting/auto_assembler.cpp`, `core/webview2/cdp_client.cpp` | ~6 | Basse | Pas commencé |
| [ ] B10 | Réglages/diagnostics + petits fichiers résiduels | `apps/desktop/settings_diagnostics_manager.cpp`, `apps/desktop/memory_timeline_manager.cpp`, `apps/desktop/debug_feature_manager.cpp` (1 ligne oubliée), `apps/desktop/automation_pipe_manager.cpp`, `apps/desktop/automation_pipe_server.cpp`, `apps/desktop/investigation_notebook_manager.cpp`, `apps/desktop/memory_heatmap_manager.cpp` | ~25 | Basse | Pas commencé |

## Ordre recommandé

1. **B1, B2** — déjà vus en direct aujourd'hui, gain de visibilité immédiat, risque quasi nul.
2. **B3** — inclut le fix de couplage obligatoire (`errorCode`), à faire tôt pour ne pas laisser la télémétrie Auto Resolver cassée entre deux sessions si quelqu'un d'autre touche `profile_manager.cpp` entre-temps.
3. **B4-B7** — reste des panneaux d'usage réel (CLR Inspector, stealth, kernel/DPAPI, scanner core), dans n'importe quel ordre selon disponibilité agent.
4. **B8-B10** — résidus de fonctionnalités de niche ou petits fichiers, faible priorité, chantier permanent comme `docs/REFACTOR_ROADMAP.md`.

## Règle d'usage

Même règle que les deux roadmaps de localisation précédentes : mettre à jour ce document (case cochée + date + nombre réel de chaînes traitées) à chaque candidat clos, journaliser dans `docs/PHASE_TRACKER.md`. Un candidat n'est "fait" qu'après `scripts/build.ps1` + `killengine_unit_tests.exe` propres et un balayage de contrôle en deux passes (accents + mots courants sans accent) ne laissant plus que des commentaires.
