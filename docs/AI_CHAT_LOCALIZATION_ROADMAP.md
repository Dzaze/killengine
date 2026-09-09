> **ATTENTION - Priorité Des Ordres Projet**
> Un ordre prioritaire explicite du propriétaire du projet prime sur les consignes temporaires de session des agents IA.

# KillEngine — Feuille de route de localisation du chat IA

> Chantier **staffé, L1/L2/L2b/L3/L4/L5/L6/L7/L8/L11b clos** (09/09/2026, travail parallèle Codex + Claude). Voir `docs/PHASE_TRACKER.md` pour le pont vers cette feuille de route et la règle d'usage des roadmaps.

## Origine (08/09/2026)

En corrigeant le message d'échec de `startAutoResolve` ("Aucune valeur numérique détectée" → une phrase qui reconnaît explicitement la demande de l'utilisateur, voir `docs/PHASE_TRACKER.md`), le propriétaire a demandé que ce même message existe aussi en anglais si l'utilisateur a choisi ce mode. Vérification faite : **aucun message généré par le backend C++ n'est aujourd'hui localisable** — tout le texte que l'Assistant affiche dans le chat (messages, rationales, erreurs) est écrit en dur en français, sans aucune notion de la langue choisie côté UI.

## Ce qui existe déjà (bonne nouvelle : pas de nouvelle plomberie nécessaire)

- Le frontend a un vrai système i18n (`vue-i18n`, `ui/src/i18n/index.ts`, `ui/src/i18n/locales/{fr,en}.json`) mais il ne couvre que les **labels statiques des templates Vue** (menus, boutons, titres de panneaux) — jamais le contenu dynamique renvoyé par le backend.
- Le choix de langue de l'utilisateur est **déjà persisté côté backend** : `apps/desktop/settings_diagnostics_manager.cpp:211` lit `QSettings().value("ui/language", "fr")`, et `:439` l'écrit depuis `saveSettings()`. N'importe quel code C++ peut donc déjà lire `QSettings().value("ui/language", "fr").toString()` sans avoir besoin d'un nouveau canal IPC.
- **Le modèle local (llama.cpp/Qwen) ne génère jamais de texte libre visible par l'utilisateur** : son prompt (`ai/llama_runtime.cpp::buildPrompt`) le contraint à ne produire qu'un objet JSON `{"tool":...,"args":...}` (schéma strict, tout texte hors JSON est rejeté par `extractToolCallJson`). Tout le texte conversationnel affiché dans le chat (les champs `message`/`rationale`/`error`) est donc **construit déterministiquement en C++**, jamais généré par le LLM lui-même. Ça simplifie beaucoup le problème : pas besoin de faire répondre le modèle "dans la bonne langue", juste de traduire des templates de chaînes C++.
- Le backend IA externe (Claude, `ai/claude_backend_client.cpp`) est un cas à part : lui génère du texte libre réel. Il s'adapte probablement déjà à la langue de la conversation, mais ça reste à vérifier — voir "Décisions à prendre" plus bas.

## Ampleur réelle — RÉVISÉE À LA HAUSSE (audit initial 08/09/2026, corrigé le même jour après démarrage de L3)

**Le premier audit (ci-dessous, barré) sous-estimait fortement le scope.** Il comptait uniquement les affectations directes `["message"] =` / `["rationale"] =` / `["error"] =`, mais un pattern très fréquent lui échappait entièrement : des chaînes françaises passées comme **argument de fonction** plutôt qu'affectées à un champ — ex. `ai/ai_engine.cpp::makeToolCall(tool, args, "Rationale en français")`, appelé **87 fois** rien que dans ce fichier. Idem très probablement dans `smart_search_manager.cpp` (le plus gros fichier) et ailleurs.

Deuxième audit, plus fiable : compte des **lignes contenant au moins une chaîne avec un caractère accentué français** (proxy imparfait mais qui attrape aussi bien les affectations que les arguments de fonction et les littéraux `{"label", "..."}`) :

| Fichier | Lignes avec texte FR (2ᵉ audit) | Occurrences affectations (1ᵉʳ audit, sous-estimé) | Visible chat ? |
| --- | --- | --- | --- |
| `apps/desktop/smart_search_manager.cpp` | **371** | 167 | **Oui, cœur du dispatch chat/IA** |
| `apps/desktop/application_controller.cpp` | 235 | 215 | Partiel — mélange chat + autres panneaux UI |
| `ai/ai_engine.cpp` | 188 | 23 | **Oui, cœur du dispatch IA** — en cours (voir Progrès) |
| `apps/desktop/claude_chat_manager.cpp` | **98 — absent du 1ᵉʳ audit** | — | Oui — backend IA externe (Claude) |
| `apps/desktop/debug_feature_manager.cpp` | 65 | 83 | Oui |
| `apps/desktop/scanning_core_manager.cpp` | 44 | 86 | Oui |
| `ai/investigation_notebook_planner.cpp` | **40 — absent du 1ᵉʳ audit** | 5 | À vérifier |
| `apps/desktop/profile_manager.cpp` | 37 | 110 | Partiel |
| `apps/desktop/display_string_investigator.cpp` | 25 | 50 | Oui |
| `apps/desktop/code_patch_manager.cpp` | 24 | 41 | Oui |
| `apps/desktop/write_freeze_core_manager.cpp` | 22 | 35 | Oui |
| `apps/desktop/save_file_investigator.cpp` | 22 | 28 | Oui |
| `apps/desktop/kernel_driver_manager.cpp` | 22 | 25 | Oui |
| `apps/desktop/freeze_hotkey_overlay_manager.cpp` | 10 | 9 | Oui |
| `apps/desktop/settings_diagnostics_manager.cpp` | 8 | 16 | Non |
| `apps/desktop/lua_repl_manager.cpp` | 7 | 8 | Oui |
| `apps/desktop/automation_pipe_server.cpp` | 7 | 4 | Non |
| `ai/claude_backend_client.cpp` | 5 | 6 | Backend externe |
| `apps/desktop/external_tool_profiler.cpp` | 4 | 8 | Oui |
| `apps/desktop/clr_inspector_bridge.cpp` | 0 (chaînes sans accent) | 34 | Non |
| `apps/desktop/memory_timeline_manager.cpp` / `investigation_notebook_manager.cpp` / `automation_pipe_manager.cpp` | 0 chacun | ~6 cumulés | Non/mineur |

**Total 2ᵉ audit : ~1234 lignes** avec au moins une chaîne française détectée (sous-compte les chaînes sans accent comme "oui"/"non" courtes, sur-compte potentiellement les lignes à 2 chaînes courtes — donc un ordre de grandeur, pas un nombre exact de chaînes individuelles). Compte tenu que chaque "ligne" peut être le début d'une chaîne de plusieurs lignes ou contenir 2 chaînes courtes, le nombre réel de chaînes individuelles à traduire est probablement **quelque part entre 1200 et 2000**, contre les ~960 *affectations* comptées la première fois (qui elles-mêmes ne couvraient qu'une fraction des chaînes réelles). **Constat honnête : impossible d'avoir un chiffre exact sans traiter fichier par fichier — les deux audits sont des ordres de grandeur, pas des inventaires.**

<details>
<summary>Premier audit (08/09/2026, sous-estimé — gardé pour traçabilité)</summary>

Comptage des affectations `["message"] =` / `["rationale"] =` / `["error"] =` uniquement :

| Fichier | Occurrences | Visible chat ? |
| --- | --- | --- |
| `apps/desktop/application_controller.cpp` | 215 | Partiel |
| `apps/desktop/smart_search_manager.cpp` | 167 | Oui |
| `apps/desktop/profile_manager.cpp` | 110 | Partiel |
| `apps/desktop/scanning_core_manager.cpp` | 86 | Oui |
| `apps/desktop/debug_feature_manager.cpp` | 83 | Oui |
| `apps/desktop/display_string_investigator.cpp` | 50 | Oui |
| `apps/desktop/code_patch_manager.cpp` | 41 | Oui |
| `apps/desktop/write_freeze_core_manager.cpp` | 35 | Oui |
| `apps/desktop/clr_inspector_bridge.cpp` | 34 | Non |
| `apps/desktop/save_file_investigator.cpp` | 28 | Oui |
| `apps/desktop/kernel_driver_manager.cpp` | 25 | Oui |
| `ai/ai_engine.cpp` | 23 | Oui |
| `apps/desktop/settings_diagnostics_manager.cpp` | 16 | Non |
| Reste (11 fichiers) | ~50 cumulés | Mixte |

Total brut : ~960 affectations sur 23 fichiers — **ce chiffre ratait entièrement les chaînes passées en argument de fonction (ex: `makeToolCall(tool, args, "rationale FR")`), un pattern très fréquent découvert seulement en commençant la migration de `ai/ai_engine.cpp`.**

</details>

## Progrès (08/09/2026)

- **L1 (fondation) — fait, testé.** `core/localization/localization.{h,cpp}` : `killcore::currentUiLanguage()` (lit `QSettings "ui/language"`, déjà écrit par `saveSettings()`) + `killcore::localizedText(fr, en)` + macro `KE_TXT(fr, en)`. Bonus trouvé en testant : `tests/unit/test_main.cpp` ne configurait aucun `QCoreApplication`/org+app name, donc **`QSettings()` ne pouvait jamais écrire nulle part dans les tests** (`status() == AccessError`, masqué jusqu'ici car les tests existants touchant QSettings avaient toujours un filet de sécurité par variable d'environnement en parallèle) — corrigé (`QCoreApplication` + org/app "KillEngine" enregistrés via un `::testing::Environment` global). 5 nouveaux tests (`tests/unit/test_localization.cpp`), 468/468 tests passent au total.
- **`ai/ai_engine.cpp` (L3) — fait, testé (09/09/2026).**
  - Playbook d'enquête (`makeInvestigationPlaybookResponse`, struct `Entry` restructurée en paires `{fr, en}` par champ, 9 entrées × 7 champs) — fait.
  - `recoveryActionsForTopic` (labels de boutons) — fait.
  - Toutes les affectations directes `["message"]`/`["rationale"]`/`["error"]` du 1ᵉʳ audit (~20 chaînes) — faites.
  - **~84 rationales `makeToolCall(...)` restantes** (chaînes en argument de fonction, découvertes en cours de route) — **faites** : toutes les fonctions `matchXxxTool` (Trainer, field-stability, WebView2/CDP, auto-report, UI-sources, AOB/suggest-patch/disassemble-backward) et les deux fonctions `deterministicPlan`/`deterministicPlanWithContext` (freeze, kernel_write, speedhack, network block, HTTP proxy, DNS spoof, lag switch, stealth mode, write_value, prepare_write_checkpoint, unknown_capture/compare, trace_ui_string, encrypted_scan, next_scan). Balayage final du fichier (grep accentué) : seul littéral français restant est `formatReminder` (ligne ~1138), une instruction envoyée au modèle en retry, pas du texte affiché à l'utilisateur — laissé tel quel délibérément.
- **`smart_search_manager.cpp` (L2) — fait, testé (09/09/2026).** 411 occurrences localisées via `KE_TXT` : messages, erreurs, rationales, confirmations, libellés, recommandations et arguments de fonctions (`addStrategyScore`, `proactiveAction`, étapes exécutées, confirmations chat). Audit étendu aux chaînes sans accents, aux littéraux multilignes et aux verbes courts assemblés. Les mots-clés de reconnaissance FR/EN restent inchangés.
- **Intégration (09/09/2026, Claude)** : Codex a travaillé sur une branche locale (`agent/ai-chat-localization-l2`, commit `947e624`) pendant que L3 se terminait dessus dans le même dossier de travail (branche partagée, pas de worktree séparé — collision de branche évitée de justesse, fichiers disjoints donc aucun conflit réel). L3 committé par-dessus (`a756c31`), fusionné en fast-forward sur `main`, branche temporaire supprimée. Build complet + **468/468 tests** sur `main` fusionné. **Vérifié en live** via le pipe d'automatisation : bascule réelle `saveSettings({"language":"en"})`, confirmé que les messages du chat (garde-fou "aucun processus attaché", clarification sociale "salut") s'affichent bien en anglais de bout en bout, y compris via le chemin de repli déterministe après échec du modèle. Poussé sur `origin/main` (`2e2dd33..a756c31`).
- **`code_patch_manager.cpp` (L8) + `ai/investigation_notebook_planner.cpp` (L11b) — faits, testés (09/09/2026, Claude).** Travaillés dans le dossier principal pendant que Codex faisait L9+L10 dans un worktree séparé (`../killengine-codex-l9l10`). L8 : ~19 chaînes (erreurs adresse/patch/hook/auto-assembler, hint de protection UWP/Store, avertissements de signature). L11b : ~24 chaînes (hypothèses et `nextTest` du plan d'enquête déterministe — `fallbackNextTest`, `makeFallbackInvestigationNotebookPlan`) ; le prompt envoyé au modèle (`buildInvestigationNotebookPlanPrompt`) reste volontairement en français, même logique que `formatReminder` dans `ai_engine.cpp` (instruction interne au modèle, jamais affichée). **Bug trouvé au passage** : `extractInvestigationNotebookPlanJson` avait la même boucle infinie potentielle que celle corrigée précédemment dans `llama_runtime.cpp` (`text.lastIndexOf('{', start - 1)` avec `start == 0` redémarre la recherche depuis la fin au lieu de s'arrêter) — corrigée avec le même correctif (`if (start == 0) break;`), test de non-régression ajouté (`ExtractJsonPlanDoesNotHangOnUnterminatedJsonAtStart`). Build complet + **469/469 tests**. Vérifié en live via le pipe d'automatisation : `scanAobPattern` sans processus attaché → `"No process attached."` en anglais ; `proposeInvestigationNotebookPlan` en mode déterministe (`useModel:false`) → hypothèses/nextTest intégralement en anglais puis en français après rebascule de la langue.
- **`scanning_core_manager.cpp` (L4) + `write_freeze_core_manager.cpp` (L5) — faits, testés (09/09/2026, Claude).** Travaillés en parallèle de Codex sur L2b (worktree git séparé cette fois, `../killengine-codex-l2b`, leçon retenue de la collision de branche précédente). Même surprise que sur L3 : le grep accentué initial (44/22 lignes) ratait un nombre important de chaînes courtes sans accent ("Type invalide.", "Adresse invalide.", "Mode invalide.", etc.) — repéré via une recherche plus large par mot capitalisé, ~65 et ~30 chaînes réellement traduites au total. Un vrai bug de copier-coller trouvé et corrigé en cours de route (un `replace_all` avait matché un fragment de texte déjà traduit, produisant un `KE_TXT(KE_TXT(...), ...)` imbriqué invalide — repéré immédiatement en revérifiant après coup, corrigé avant le build). Build complet + **468/468 tests**.

## Recommandation pour la suite (vu l'ampleur réelle)

Compte tenu du volume réel (1200-2000+ chaînes, pas ~960), traduire à la main fichier par fichier au fil d'une seule session n'est pas réaliste — c'est cohérent avec le principe déjà appliqué à `docs/REFACTOR_ROADMAP.md` : **chantier permanent à faible priorité, grignoté au fil des sessions futures, jamais un blocage pour une phase produit en cours.** Deux leviers pour rendre ça soutenable :

1. **Prioriser par fréquence réelle d'apparition dans le chat plutôt que par ordre de fichier.** Beaucoup de ces ~1200-2000 chaînes sont des cas d'erreur rares ou des branches secondaires jamais/rarement exercées par un utilisateur réel. Une meilleure approche que "traduire le fichier dans l'ordre" : instrumenter (ou simplement observer sur plusieurs sessions réelles) quels messages apparaissent le plus souvent dans `smart_search_debug.jsonl`/les logs de chat, et traduire d'abord le top 20-50 messages les plus fréquents — probablement 80% de l'exposition utilisateur réelle pour une fraction du volume total.
2. **Détecter les patterns "fonction-avec-argument-français" avant de commencer un nouveau fichier**, pas seulement les affectations — sinon chaque nouveau fichier réserve la même mauvaise surprise que `makeToolCall`. Un grep rapide du type `grep -cP '"[^"]*[àâäéèêëïîôöùûüçÀÂÄÉÈÊËÏÎÔÖÙÛÜÇ][^"]*"' <fichier>` avant de s'engager donne un ordre de grandeur correct (voir tableau ci-dessus).

## Décisions déjà prises

1. **Mécanisme de traduction côté C++ — tranché et livré (L1)** : `KE_TXT(fr, en)` (macro, `core/localization/localization.h`), pas de fichiers `.ts` Qt Linguist (trop lourd pour du texte avec interpolation `.arg()`). Pour les structures de données littérales (ex: le playbook d'enquête, `Entry` dans `ai_engine.cpp`), chaque champ devient une paire `{fr, en}` plutôt qu'un seul `const char*`, choisie au point de consommation via `killcore::localizedText(...)`.
2. **Backend Claude externe** : textes déterministes de `claude_chat_manager.cpp` localisés (L2b, 09/09/2026). Le prompt système reste interne et inchangé ; la langue du texte libre généré par Claude reste à vérifier en live.
3. **Ordre de priorité** : confirmé, `ai_engine.cpp` (L3) avant `smart_search_manager.cpp` (L2) — en cours, voir "Progrès" ci-dessus.
4. **Granularité de migration** : confirmé fichier par fichier façon `docs/REFACTOR_ROADMAP.md`. **Mise à jour post-audit corrigé** : vu l'ampleur réelle, prévoir aussi un découpage PAR PATTERN à l'intérieur d'un même fichier (ex: dans `ai_engine.cpp`, "affectations directes" et "arguments `makeToolCall`" ont été deux passes distinctes) plutôt que de viser un fichier "fini" en une fois.

## Candidats (mis à jour avec l'ampleur corrigée — occurrences = lignes avec texte FR détecté, 2ᵉ audit)

| # | Candidat | Fichier(s) | Occurrences (corrigées) | Priorité | Statut |
| --- | --- | --- | --- | --- | --- |
| [x] L1 | Fondation : helper de traduction + lecture `ui/language` | `core/localization/localization.{h,cpp}` | — | **Bloquant** | **Fait, testé (08/09/2026)** |
| [x] L3 | Coordination modèle (intents, retries, rejets, playbook d'enquête, fallback déterministe) | `ai/ai_engine.cpp` | 188 (affectations + playbook + ~84 rationales `makeToolCall`) | **Haute** | **Fait, testé (09/09/2026)** |
| [x] L2 | Dispatch chat/IA (auto_resolve, exact_scan, next_scan, erreurs génériques) | `apps/desktop/smart_search_manager.cpp` | **411 occurrences traitées** (audit initial : 371 lignes accentuées) | **Haute** — cœur de la demande initiale | **Fait, testé (09/09/2026)** |
| [x] L2b | Backend IA externe (Claude) — chat | `apps/desktop/claude_chat_manager.cpp` | **51 occurrences traitées** (audit initial : 98 lignes accentuées, prompt système inclus) | Haute (même famille que L2/L3) | **Fait, testé (09/09/2026)** |
| [x] L4 | Outils scan (exact/next/unknown/groupe/chiffré) | `apps/desktop/scanning_core_manager.cpp` | 44 lignes détectées, ~65 chaînes traitées (dont plusieurs sans accent ratées par le grep initial : "Type de valeur invalide.", "Type invalide.", "Adresse de fin invalide.", etc.) | Moyenne | **Fait, testé (09/09/2026)** |
| [x] L5 | Write/freeze | `apps/desktop/write_freeze_core_manager.cpp` | 22 lignes détectées, ~30 chaînes traitées (même constat : plusieurs sans accent, ex. "Adresse invalide.", "Type invalide.", "Valeur invalide.") | Moyenne | **Fait, testé (09/09/2026)** |
| [x] L6 | Debug/breakpoints/speedhack (`find_what_writes` etc.) | `apps/desktop/debug_feature_manager.cpp` | 65 lignes détectées, ~50 chaînes traitées en 2 passes (un premier balayage large a raté ~10 chaînes de plus, ex. "Impossible d'ouvrir le processus avec les droits nécessaires..." répétée 5×, "Échec du réglage du facteur.") | Moyenne | **Fait, testé (09/09/2026)** |
| [x] L7 | Trace UI string / analyse sources | `apps/desktop/display_string_investigator.cpp` | 25 lignes détectées, ~25 chaînes traitées | Moyenne | **Fait, testé (09/09/2026)** |
| [x] L8 | Patch de code / AOB | `apps/desktop/code_patch_manager.cpp` | 24 lignes détectées, ~19 chaînes traitées | Basse | **Fait, testé (09/09/2026)** |
| [ ] L9 | Save-file / UWP | `apps/desktop/save_file_investigator.cpp` | 22 | Basse | Pas commencé — **en cours chez Codex (worktree `../killengine-codex-l9l10`)** |
| [ ] L10 | Kernel write | `apps/desktop/kernel_driver_manager.cpp` | 22 | Basse | Pas commencé — **en cours chez Codex (worktree `../killengine-codex-l9l10`)** |
| [ ] L11 | Profils / pointer chains (sous-ensemble visible chat uniquement) | `apps/desktop/profile_manager.cpp` | 37 (à cribler) | Basse | Pas commencé |
| [x] L11b | Investigation notebook planner | `ai/investigation_notebook_planner.cpp` | 40 lignes détectées, ~24 chaînes traitées | Basse-moyenne | **Fait, testé (09/09/2026)** |
| [ ] L12 | Reste (`application_controller.cpp` — 235, `settings_diagnostics_manager.cpp`, `freeze_hotkey_overlay_manager.cpp`, `lua_repl_manager.cpp`, `automation_pipe_server.cpp`, `claude_backend_client.cpp`, `external_tool_profiler.cpp`, `clr_inspector_bridge.cpp`...) — à cribler pour isoler le sous-ensemble réellement visible en chat vs panneaux dédiés déjà couverts par l'i18n statique | multiple | ~330 cumulés | Basse / à évaluer si nécessaire du tout | Pas commencé |

**Ordre recommandé (mis à jour le 09/09/2026)** : L1, L2, L2b, L3, L4, L5, L6 et L7 clos — le cœur du chat (Assistant local ET backend Claude externe), les outils de scan/write/freeze, debug/breakpoints/speedhack et trace UI string sont intégralement bilingues FR/EN. Deuxième round de travail parallèle : Codex sur L2b (worktree git séparé, `../killengine-codex-l2b`, pour éviter la collision de branche du round précédent) pendant que Claude faisait L4+L5 dans le dossier principal — fusionnés sans conflit de code (un seul conflit, sur ce fichier de roadmap lui-même, résolu à la main). L6 puis L7 faits seul juste après, avec **deux passes de balayage** à chaque fois. **Troisième round parallèle** : Codex sur L9+L10 (worktree séparé `../killengine-codex-l9l10`) pendant que Claude faisait L8+L11b dans le dossier principal — fichiers disjoints, aucun risque de collision. **Bonus L11b** : la même classe de bug d'infini que celui corrigé dans `llama_runtime.cpp` (`QString::lastIndexOf(ch, -1)` qui redémarre depuis la fin au lieu de s'arrêter) existait aussi dans `extractInvestigationNotebookPlanJson` — corrigée au passage, test de non-régression ajouté. L11/L12 à ré-évaluer une fois L9/L10 clos côté Codex : possible qu'une bonne partie de leur contenu ne soit jamais montrée dans le chat.

### Validation L2 — 09/09/2026 (Codex)

**Diagnostic → correctif** : le dispatcher construisait ses textes en français indépendamment de `ui/language`. Les 411 sites utilisent maintenant la fondation L1, y compris les arguments de fonctions, les messages multilignes et les fragments de phrases. Aucun identifiant d’outil, clé JSON, mot-clé de reconnaissance ou chemin de confirmation/exécution n’a changé. Les textes reçus des autres managers restent dans le périmètre de leurs lots respectifs.

**Tests → validation** : `scripts/build.ps1` réussi ; `build/bin/killengine_unit_tests.exe` : **468/468**. Vérification statique des placeholders sur les 411 paires et comparaison des tokens C++ après restitution de la branche française : identiques à la base, hors constructeurs `QString` devenus redondants. Le grep accentué brut retourne encore **370 lignes** (le français est conservé dans `KE_TXT`) ; après exclusion des macros, les **44 lignes restantes** sont exclusivement des mots-clés de reconnaissance et des commentaires, aucun texte français affiché oublié. Audit supplémentaire des chaînes sans accent effectué. UTF-8 sans BOM et CRLF du fichier C++ conservés ; aucun mojibake introduit ; rapport global de fins de ligne exécuté, anomalies historiques hors périmètre conservées. Pas de validation visuelle live du chat. `docs/PHASE_TRACKER.md` laissé au propriétaire, selon sa demande.

### Validation L2b — 09/09/2026 (Codex)

**Diagnostic → correctif** : 51 occurrences localisées via `KE_TXT` dans `claude_chat_manager.cpp` : erreurs de clé API/backend, délais dépassés, erreurs des outils et de scan, confirmations write/freeze/kernel/speedhack/réseau/HTTP/DNS/stealth/WebView2 et fragment « Arrêt du freeze ». Les arguments de `makeErrorResult` et `requestConfirmation`, les chaînes multilignes et les textes sans accents sont inclus. `kSystemPrompt` reste strictement inchangé : instruction interne au modèle, hors périmètre de traduction. Les erreurs renvoyées par les autres managers et le texte libre généré par Claude restent dans leurs périmètres respectifs.

**Audit** : grep accentué brut = **98 lignes** ; après exclusion des 51 macros, les **37 lignes restantes** appartiennent exclusivement à `kSystemPrompt`. Aucun texte français affichable oublié dans ce fichier. Placeholders identiques pour chaque paire ; restitution des branches françaises et comparaison des tokens C++ avec la base : identiques, hors constructeurs `QString` redondants. Encodage UTF-8 sans BOM et fins de ligne du worktree conservés. `docs/PHASE_TRACKER.md` laissé au propriétaire selon sa demande.

**Tests → validation** : configuration propre puis `scripts/build.ps1` réussi (307 étapes) dans le worktree isolé `agent/ai-chat-localization-l2b` ; `build/bin/killengine_unit_tests.exe` : **468/468 tests passent**. `git diff --check` propre ; contrôle global des fins de ligne : 448/448 fichiers CRLF ; scan mojibake : uniquement les exemples historiques documentés. Le worktree neuf ne contient pas les exécutables llama ni les poids GGUF (avertissements de staging du modèle local, sans échec du build). Pas de requête réelle à l’API Anthropic ni de validation visuelle live du chat.
