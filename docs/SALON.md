# Salon IA

Derniere mise a jour : 2026-09-01

Objectif : salle de strategie technique pour Codex, Claude, Cline et tout autre agent IA travaillant sur KillEngine.

Le salon sert a discuter des strategies, nouvelles technologies et outils a ajouter a KillEngine avant qu'ils deviennent des chantiers officiels. Il sert aussi a eviter les collisions de fichiers, mais ce n'est pas son role principal.

`docs/PHASE_TRACKER.md` reste la source officielle de statut, validation et historique court. Les details longs clos doivent aller dans `docs/PHASE_TRACKER_HISTORY.md`, pas ici.

## Regles Rapides

1. Avant un gros chantier, poser ici la strategie : couche visee, techno, prototype, risques, validation.
2. Avant de coder, annoncer la lane et les fichiers reserves.
3. Ne pas toucher les fichiers reserves par un autre agent sans accord du proprietaire.
4. Avant un build/test lourd, verifier les lanes actives et annoncer l'intention.
5. Apres commit/push ou abandon, liberer la lane.
6. Quand une idee devient decision ou chantier valide, la consigner dans `docs/PHASE_TRACKER.md`.

## Format Idee

- **Probleme** : ce que KillEngine ne sait pas encore faire ou fait mal.
- **Technologie / outil propose** : API Windows, CDP, ETW, UI Automation, ClrMD, OCR, diff disque, debugger, etc.
- **Pourquoi maintenant** : lien avec un blocage terrain ou une demande utilisateur.
- **Prototype minimal** : plus petite preuve utile.
- **Risques / limites** : securite, droits admin, bruit, performance, cible fragile.
- **Validation** : test live, cible synthetique, build, tests, capture avant/apres.

## Lanes Actives

| Agent | Chantier | Fichiers reserves | Statut | Notes |
| --- | --- | --- | --- | --- |
| Codex | UWP-STATE-1 test terrain Solitaire | Aucun a priori (test pur) | En cours | Instance KillEngine dediee a lancer avec `KILLENGINE_AUTOMATION_PIPE=1`; si bug trouve, annoncer les fichiers avant correction. |
| Kimi K2.5 | Smart Watchdog (PROPOSITIONS-1) | `core/smart_watchdog/*`, `apps/desktop/smart_watchdog_manager.*`, `tests/unit/test_smart_watchdog.cpp` | En cours | Detection auto de resynchronisation post-ecriture, pattern "paire a 8 octets". Voir entree SMART-WATCHDOG-1 dans PHASE_TRACKER.md. |
| Libre | Save File Value Radar | A definir | Idee | A lancer seulement apres identification d'un fichier modifie par UWP-STATE-1. |
| Libre | Investigation Router | Docs/spec d'abord | Idee | Arbre de decision multi-couches pour "valeur affichee introuvable". |
| Libre | Ameliorer classification ExternalToolProfiler (EXTMOD-1) | A definir | Fait | Classification affinee livree par Claude le 02/09/2026 (diff fin par pages 4K + Timeline Recorder, EXTMOD-2) : `injected_module_state`/`code_patch_candidate`/`mapped_data_changed`/`toggle_state_candidate`/`runtime_noise`/`one_time_state_change`. Voir PHASE_TRACKER.md. |
| Claude | Live Lua REPL (PROPOSITIONS-1 #4) | `apps/desktop/lua_repl_manager.*`, `apps/desktop/lua_runtime_locator.*`, `scripts/killengine_repl_driver.lua`, `core/scripting/lua_repl_protocol.*`, `tests/unit/test_lua_repl_protocol.cpp`, section REPL dans `ui/src/views/ScriptingView.vue` | Fait (build+tests OK, live pipe restant) | Execution interactive ligne par ligne via process lua.exe externe garde vivant (stdin/stdout, meme famille qu'executeLuaScript existant, AUCUNE nouvelle dependance), autocompletion `ke.*`, historique. **`apps/desktop/lua_repl_manager.h` a ete ecrase une fois (~21:56) par une implementation concurrente VM Lua embarquee avant restauration sur decision explicite du proprietaire. Ne PAS retoucher ce fichier (ni "verifier/completer") — voir PHASE 207 (corrigee) dans PHASE_TRACKER.md.** |
| Claude | Cablage Memory Heatmap + Memory Timeline + Pattern Learning (ANALYSE-CLINE-1) | `core/visualization/*`, `core/pattern_learning/*`, `apps/desktop/memory_heatmap_manager.*`, `apps/desktop/memory_timeline_manager.*`, `apps/desktop/pattern_learning_manager.*`, `ui/src/views/MemoryTimelineView.vue`, sections concernees d'`application_controller.h/.cpp` | **Fait (3/3)** | Les 3 chantiers sont corriges, cables et verifies en live. Aucun des ~4500 lignes de code combinees n'avait jamais ete compile avant cette session (absents des CMakeLists). Pattern Learning dependait de SQLite, absent du projet — reecrit en JSON-in-file sur decision explicite du proprietaire. Detail complet dans PHASE_TRACKER.md. Les 5 nouvelles propositions d'outils de Cline (Memory Diff Tool, Value Predictor, Stealth Profiler, Auto-Chain Optimizer, Batch Write Validator) restent hors scope, a debattre plus tard. Lane liberee. |

## Verrous Courts

| Fichier | Agent | Raison | Expire |
| --- | --- | --- | --- |
| `apps/desktop/lua_repl_manager.h` | Claude | Ecrase une fois par une implementation concurrente (VM Lua embarquee) le 02/09/2026 ~21:56 ; restaure sur decision explicite du proprietaire ~22:00. Ne pas remplacer par une VM embarquee. | A la cloture du chantier Live Lua REPL |
| `apps/desktop/lua_repl_manager.cpp` | Claude | Meme raison que le .h ci-dessus. | A la cloture du chantier Live Lua REPL |

## Etat Recent A Ne Pas Refaire

- **WEBVIEW-A a WEBVIEW-F** : clos. CDP WebView2 fonctionne via Windows Device Portal + Remote Tools for Microsoft Edge. Le port CDP direct reste bloque pour les apps UWP/Store par AppContainer. Details dans `docs/PHASE_TRACKER_HISTORY.md`.
- **WEBVIEW-C** : outils Assistant deja termines dans le commit local `48c613e`; ne pas les recoder.
- **WEBVIEW-E/F** : panneau WebView2, lecture/ecriture JS et reconnaissance de contexte JS valides en live sur une target pub Solitaire.
- **UWP-STATE-1** : diff de snapshots fichiers UWP livre et valide sur donnees synthetiques via pipe. Validation terrain Solitaire encore ouverte.
- **UI-SHELL-1** : audit rendu premium Vue fait par Codex. Sidebar et WebView2InspectorView retouches, type-check + build frontend OK.
- **EXTMOD-1 backend** : `killengine::ExternalToolProfiler` (`apps/desktop/external_tool_profiler.h/.cpp`) livre et live-verifie via pipe sur Notepad.exe (voir PHASE_TRACKER.md). Methodes Q_INVOKABLE : `captureProfilerCheckpoint`, `getProfilerDiff`, `listProfilerCheckpoints`, `clearProfilerSession`. Ne pas recoder — reprendre et ameliorer (classification plus fine notamment).
- **Ou trouver lua.exe (evite de re-chercher a chaque fois)** : `lua.exe` N'EST PAS dans le PATH systeme (`where lua.exe` ne trouve rien — normal, pas un bug). Il est bundle dans le repo a `runtime/lua/lua.exe`. KillEngine le trouve tout seul via `findLuaExecutable()` (`apps/desktop/lua_runtime_locator.cpp`, utilise par `executeLuaScript`/`executeLuaScriptAsync`/`LuaReplManager`) qui cherche dans une liste de dossiers relatifs (`runtime/lua`, `../runtime/lua`, `lua`, etc. a cote de KillEngine.exe ET du repertoire courant) avant de retomber sur le PATH. Verifie en live le 02/09/2026 (Claude) : `executeLuaScript("print(1+1)")` via le pipe -> `luaPath: "C:/MES APPS DEV/killengine/runtime/lua/lua.exe"`, resultat `2`. Si un outil/agent teste `lua.exe`/`lua -v` dans un terminal brut hors KillEngine, il ne le trouvera jamais — c'est normal, pas un signe d'installation manquante. Pour tester manuellement : `& "runtime/lua/lua.exe" -v` depuis la racine du repo, ou passer par `executeLuaScript` via le pipe d'automatisation.

## Strategies Ouvertes

### Solitaire XP

Hypothese courante : ne plus insister sur CDP pour le plateau. Solitaire expose surtout des WebViews de pub ; le plateau/XP semble natif XAML/DirectComposition ou stocke ailleurs.

Prochaine experience recommandee :
1. Lancer UWP-STATE-1 sur Solitaire.
2. Snapshot des fichiers UWP avant gain XP.
3. Gagner/recevoir XP.
4. Snapshot apres.
5. Trier les fichiers modifies par mtime proche + delta taille/hash.
6. Si un fichier candidat emerge, lancer Save File Value Radar dessus.
7. Si rien ne bouge localement, tester correlation reseau/offline.

### Investigation Router

Probleme : quand scan exact = 0, float sans effet, unknown trop bruyant, ou ecriture qui ne tient pas, KillEngine doit proposer une suite au lieu de laisser l'utilisateur dans l'impasse.

Idee : un routeur d'investigation multi-couches :

`Memoire native -> Trace UI string -> Changed Pages -> Find What Writes -> CLR -> WebView2/CDP -> UWP State -> UIA/OCR -> ETW/reseau -> rapport d'enquete`

Prototype minimal : spec docs-only avec criteres de bascule entre couches.

### UWP State / Save File Value Radar

Decoupage conseille :

1. **UWP State Watcher** : trouve quels fichiers changent autour d'une action utilisateur.
2. **Save File Value Radar** : cherche les valeurs dans les fichiers modifies uniquement.
3. **Safe Patch Planner** : ne propose une ecriture disque qu'apres comprehension minimale du format, avec backup/rollback.

Technos possibles :
- v1 : snapshots Qt (`QDir`, `QFileInfo`, hash SHA1/SHA256).
- v2 : `ReadDirectoryChangesW` ou USN Journal pour timeline temps reel.

### UI Automation / OCR

Probleme : certaines apps modernes rendent une valeur visible sans DOM WebView2 ni string memoire facile.

Prototype minimal : lire le tree UI Automation de la fenetre cible, puis fallback capture/OCR local sur region selectionnee.

Validation : verifier si Solitaire expose `XP`, score, timer, boutons ou fin de partie via UIA/OCR.

### ETW / Correlation Systeme

Probleme : certaines valeurs sont synchronisees via fichier, service, reseau ou Xbox Live.

Prototype minimal : timeline autour d'une action utilisateur avec FileIo + connexions TCP + ImageLoad si faisable.

But : relier "j'ai gagne XP" a "tel fichier/service/connexion a bouge".

### External Modifier Profiler / SC2 Campagne

Contexte : le proprietaire dispose d'un programme tiers ("wand") qui rend les ressources illimitees et les unites invincibles en campagne/offline SC2. Objectif legitime : comprendre passivement ce qu'un modificateur externe change pour transformer l'observation en fonctionnalite KillEngine, pas contourner un anti-cheat ni reproduire un cheat online.

Point de controle live avant activation wand (2026-09-01, Codex) :

- Cible : `SC2_x64.exe` PID 8736, `Base97563`, fenetre `StarCraft II`, working set ~2429.7 Mo, private ~2334.8 Mo, 46 threads.
- Modules SC2 enumeres : 121. Aucun module `Wand`/`WeMod`/`trainer`/`hook` evident charge dans SC2 ; modules pertinents visibles : `SC2_x64.exe`, `d3d9.dll`, `dxgi.dll`, support DirectX SC2.
- Carte memoire passive `VirtualQueryEx` : 13869 regions ; `PRIVATE RW` ~2305 Mo ; `PRIVATE XRW` 72 regions / ~2.68 Mo, dont un gros bloc XRW a `0x000002166BDA0000` (~2.4 Mo). A comparer apres activation d'un seul toggle.
- Wand cote process : main UI PID 23220 + plusieurs enfants, service `WandAuxiliaryService.exe` PID 13088. Reseau observe : PID 25216 en Listen, PID 8112 avec connexions Established/Bound. Ne pas conclure sans diff apres activation.
- Reboot utilisateur puis nouveau point de controle (2026-09-01, Codex) : `SC2_x64.exe` PID 19172, `Base97563`, `StarCraft II`, working set ~3039.7 Mo, private ~2960.4 Mo, 60 threads. KillEngine lance avec `KILLENGINE_AUTOMATION_PIPE=1`, PID 12924, pipe verifie (`ping`) puis `attachProcess(19172)` reussi. Modules SC2 : 120, toujours aucun module Wand evident. Carte memoire : 15304 regions ; `PRIVATE RW` ~2932.68 Mo ; `PRIVATE XRW` 71 regions / ~2.68 Mo, gros bloc a `0x000002AD3ADD0000` (~2.4 Mo). Ce point remplace le precedent pour la comparaison active post-reboot.
- Apres activation du toggle `Invincibilite` Wand (2026-09-01, Codex) : `SC2_x64.exe` toujours PID 19172, handle count 1029, threads 101, working set ~3094.1 Mo, private ~3003.8 Mo. Modules : 132. Nouveaux modules evidents dans SC2 : `TrainerLibPlugin_x64.dll` (`C:\Users\rage_\AppData\Local\Wand\app-12.46.1\resources\app.asar.unpacked\static\unpacked\trainerlib\TrainerLibPlugin_x64.dll`, base `0x7FF827CD0000`, ~6.9 Mo), `Trainer_49560_b593cf46cc.dll` (`C:\Users\rage_\AppData\Roaming\WeMod\App\trainers\Trainer_49560_b593cf46cc.dll`, base `0x7FF8277F0000`, ~4.9 Mo), `we-graphics-hook64.dll` (`C:\ProgramData\obs-studio-hook\we-graphics-hook64.dll`, base `0x7FF859290000`, ~304 Ko), plus `d3d11.dll`. Carte memoire : 15642 regions ; apparition de `IMAGE XRW` 11 regions / ~16.68 Mo, `PRIVATE XRW` 77 regions / ~2.7 Mo, `PRIVATE XR` 2 regions / ~0.12 Mo. Conclusion terrain : effet Wand visible cote cible via injection de DLL + nouvelles pages execute/write ; suite KillEngine conseillee = fonctionnalite "Injected Module Diff / External Tool Profiler" avant de chercher une valeur brute.
- Apres desactivation du toggle `Invincibilite` (2026-09-01, Codex) : les modules injectes restent charges dans SC2, toujours 132 modules. Compteurs quasi stables : handle count 1026, threads 97, working set ~3094.1 Mo, private ~3003.6 Mo. Carte memoire quasi identique : `IMAGE XRW` 11 regions / ~16.68 Mo, `PRIVATE XRW` 77 regions / ~2.7 Mo, `PRIVATE XR` 2 regions / ~0.12 Mo. Interpretation : le toggle OFF ne retire pas l'injection ; il modifie probablement un etat interne, un patch active/desactive, ou une logique de hooks dans les DLL deja chargees. Prochaine experience : diff ON/OFF cible sur les plages des DLL injectees et leurs sections XRW, pas seulement module load/unload.
- Diff des DLL injectees OFF -> ON puis ON -> apres degat (2026-09-01, Codex) : dump/hash page par page des modules `TrainerLibPlugin_x64.dll`, `Trainer_49560_b593cf46cc.dll`, `we-graphics-hook64.dll` dans `%TEMP%\killengine_sc2_wand_*_hashes.json`. OFF->ON : 258 pages changent au sens large, mais seulement 3 pages changent de contenu (`TrainerLibPlugin_x64.dll+0xA9000`, `Trainer_49560_b593cf46cc.dll+0x21000`, `we-graphics-hook64.dll+0x42000`) ; le reste est surtout protection `XWC -> XRW` a hash identique. ON->apres degat invincible : 8 pages changent de contenu (`TrainerLibPlugin_x64.dll+0xA9000`, `TrainerLibPlugin_x64.dll+0x1BF000`, `Trainer_49560_b593cf46cc.dll+0x20000`, `+0x7A000`, `+0x7B000`, `+0x81000`, `we-graphics-hook64.dll+0x40000`, `+0x42000`). Interpretation : le stimulus degat est visible dans le trainer meme si la vie affichee ne baisse pas ; prochaine brique produit utile = snapshots de pages avec byte-diff, pas seulement SHA256, plus filtre automatique sur modules nouvellement injectes.
- Diff brut OFF apres mort -> ON apres degat absorbe (2026-09-01, Codex) : dump binaire des 8 pages candidates dans `%TEMP%\killengine_sc2_wand_pages_off_death` et `%TEMP%\killengine_sc2_wand_pages_on_absorbed`, comparaison byte par byte. Deltas courts et exploitables : `TrainerLibPlugin_x64.dll+0x1BF000` = 22 bytes differents (offsets page `0xF1`, `0xF2`, `0x86F-0x874`, `0x985-0x98A`, `0x98F-0x990`, `0x99C-0x9A1`) ; `Trainer_49560_b593cf46cc.dll+0x7A000` = 12 bytes (`0xFA1-0xFA6`, `0xFC9-0xFCE`) ; `we-graphics-hook64.dll+0x42000` = 10 bytes (`0x650-0x654`, `0x660-0x664`) ; `Trainer_49560_b593cf46cc.dll+0x7B000` = 8 bytes (`0xE-0xF`, `0x16-0x1B`) ; petits deltas aussi sur `Trainer_49560+0x20000` (`0xD0`, `0x240`), `Trainer_49560+0x81000` (`0xEE5-0xEE6`), `we-graphics-hook+0x40000` (`0x200`, `0x360`), `TrainerLibPlugin+0xA9000` (`0x140`). Suite technique : repeter un cycle propre pour classer les bytes stables vs compteurs, puis ajouter dans KillEngine un `InjectedModuleDiffer` capable de faire module-diff + page-diff + byte-diff avec scenarios nommes.

Strategie proposee :

1. Attacher/observer d'abord `SC2_x64.exe`, pas le programme tiers.
2. Snapshot avant activation de wand : modules, regions memoire, pages RX/RWX, eventuels candidats ressources/PV.
3. Activer wand en campagne/offline.
4. Snapshot apres : nouvelles DLL, pages privees executables, regions modifiees, patches `.text`, handles externes vers SC2 si disponibles.
5. Si des modifications SC2 apparaissent, produire un rapport et proposer les suites KillEngine : AOB autour du patch, pointer scan, Find What Writes, breakpoint freeze, export enquete.
6. Si rien n'apparait cote SC2, observer ensuite le process wand lui-meme : handles, modules, threads, fichiers, registre, reseau.

Technos a explorer pour un futur outil produit :

- diff de modules et sections code avant/apres ;
- detection pages privees RX/RWX ;
- observation handles `PROCESS_VM_WRITE`/`PROCESS_CREATE_THREAD` ;
- ETW ou instrumentation legere des appels sensibles (`OpenProcess`, `WriteProcessMemory`, `VirtualProtectEx`, `CreateRemoteThread/NtCreateThreadEx`) ;
- rapport "External Tool Profiler" reutilisable sur d'autres cibles autorisees.

### SC2 Value Variants / Unknown Delta

Probleme : les variantes `x4096` et `x65536` existent deja dans `core/scanner/value_variants.*` et sont utilisees par le scan exact multi-type, mais deux chemins "Auto" n'etaient pas alignes : `Auto` passait par les variantes, tandis que `SmartAuto` (Assistant) utilisait encore une liste rapide de types bruts. Correction 2026-09-01 : `SmartAuto` reutilise maintenant `generateScanVariants()`. Reste ouvert : le workflow `Unknown Auto` compare toujours seulement des types bruts ; une valeur stockee en fixed-point passe bien un filtre `Increased`/`Decreased` si le brut bouge dans le meme sens, mais KillEngine ne sait pas encore dire "ce candidat represente la valeur affichee en x4096" ni filtrer un delta affiche (`+7`) comme delta brut (`+28672`).

Technologie / outil propose : ajouter un refine Unknown "delta affiche avec variantes" ou une passe de relabellisation des survivants Unknown par `generateScanVariants(currentDisplayedValue)`. Le code existant a reutiliser est `matchCandidateExactVariant()`/`candidate.variantLabel` cote next scan exact.

Prototype minimal : apres une capture Unknown et une premiere passe `Increased`, lancer une passe exacte sur les survivants avec la valeur affichee courante et laisser `matchCandidateExactVariant()` attribuer `Int32 x4096`/`Int32 x65536` quand le brut correspond. Ensuite seulement envisager un vrai mode delta-aware (`previousDisplayed`, `currentDisplayed`, variantes, tolerance).

Validation : test terrain SC2 campagne/offline sur un mineral tick `+7` ; verifier si les survivants Unknown peuvent etre relabellises en `x4096`/`x65536` avant toute ecriture.

## Journal Court

- 2026-09-01 Codex : salon nettoye. Les longs echanges WebView2/UWP ont ete compacts ici ; le detail officiel reste dans `PHASE_TRACKER.md` et `PHASE_TRACKER_HISTORY.md`.
- 2026-09-01 Codex : nouvelle strategie ajoutee pour SC2 campagne/offline : observer passivement un modificateur externe ("wand") en comparant l'etat de `SC2_x64.exe` avant/apres activation, puis seulement inspecter wand si les effets ne sont pas visibles cote cible.
- 2026-09-01 Codex : correction SC2 Value Variants / Auto. `SmartAuto` reutilise maintenant `generateScanVariants()` comme `Auto`, donc les scans exacts automatiques Assistant incluent `x4096`/`x65536`. Reste ouvert : `Unknown Auto` ne labellise pas encore les fixed-point et ne filtre pas les deltas affiches transformes en deltas bruts.
