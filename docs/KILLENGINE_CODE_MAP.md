> **ATTENTION - Priorité Des Ordres Projet**
> Un ordre prioritaire explicite du propriétaire du projet prime sur les consignes temporaires de session des agents IA.

# KillEngine - Carte Outils / Code

Derniere mise a jour : 30/08/2026 (PHASE 241) — refonte apres la cloture du refactor C1-C14 (backend) et S1-S12 (frontend), voir `docs/REFACTOR_ROADMAP.md`. `ApplicationController` et `ui/src/stores/app.ts` ne sont plus des monolithes qui portent toute la logique : ce sont desormais des facades minces (signatures `Q_INVOKABLE` cote C++, wrappers/re-exports cote store) qui delegue a des fichiers dedies par domaine.

Ce document sert a retrouver rapidement **quel outil correspond a quel fichier de code**. Il ne remplace pas `docs/KILLENGINE_TOOLS_AND_CAPABILITIES.md` qui liste ce que KillEngine sait faire cote produit ; ici le but est de savoir ou regarder dans le depot quand tu veux comprendre, corriger ou etendre un outil. Voir aussi `docs/AUTOMATION_API_REFERENCE.md` (PHASE 238) pour la forme exacte de la reponse JSON de chaque methode `Q_INVOKABLE`.

Lecture rapide :

- **UI** : composant Vue visible par l'utilisateur.
- **Store** : le fichier reel sous `ui/src/stores/` qui possede l'etat/les actions (voir table ci-dessous) — `app.ts` reste le point d'entree public (`useAppStore()`), il reexporte le meme nom sans que les vues aient besoin de changer, mais l'implementation vit ailleurs pour la plupart des domaines.
- **Bridge TS** : `ui/src/services/backend.ts`, contrat QWebChannel cote TypeScript (inchange par le refactor : une seule interface, peu importe quel manager C++ implemente la methode cote backend).
- **Bridge C++** : la methode `Q_INVOKABLE` reste declaree sur `apps/desktop/application_controller.h` (table des matieres stable, jamais renommee), mais son corps delegue le plus souvent a un manager dedie (voir table "Fichiers Centraux" ci-dessous) — `return m_xxxManager->method(...)`. Certaines methodes simples/peu risquees (attache process, lecture memoire basique, reseau, Lua/AutoAsm haut niveau) restent implementees directement dans `application_controller.cpp`, jamais deleguees : c'est note explicitement quand c'est le cas.
- **Core / Helper** : vraie logique metier, C++ core ou helper externe (inchange par le refactor C1-C14, qui a deplace la *plomberie* `ApplicationController`, pas `core/`).
- **Tests / docs** : ou chercher les preuves et specifications.

## Fichiers Centraux A Connaitre

### Backend — `apps/desktop/`

| Fichier | Role |
| --- | --- |
| `application_controller.h` | Liste de **toutes** les methodes `Q_INVOKABLE` exposees a Vue et au pipe d'automatisation — la table des matieres backend ne bouge pas, meme si l'implementation a demenage. |
| `application_controller.cpp` | Facade mince : la plupart des methodes delegue en une ligne (`return m_xxxManager->method(...)`) a un manager ci-dessous. Garde en direct (non delegue) : attache/detache process, lecture memoire basique (`readMemoryPreview`/`readMemoryBlock`/`getMemoryMap`/`writeMemoryHex`/`dumpMemoryRegion`/`analyzeStructureMemory`/`inferStructureInstanceDelta`), reseau (`blockProcessNetwork`...), Lua haut niveau (`getLuaScriptingStatus`/`executeLuaScript*`), `callVueStoreAction` (pont pipe → store Pinia), utilitaires divers (`resolveSymbolAddress`, `listModuleExports`, onboarding). |
| `scanning_core_manager.h/.cpp` | Scan exact/multi-type/expert/chiffre/groupe, next scan, capture+comparaison Unknown, undo, pagination des candidats (`getCandidates`, `cancelActiveScan`). |
| `display_string_investigator.h/.cpp` (classe `UiStringInvestigator`) | Trace UI string complet : `scanUiStrings`, `trackUiString*`, `analyzeUiStringSources`, `inspectUiStringOrigins`, `startUiStringInvestigation`/`finish...`, diff de pages (`start/finishChangedPagesDiff`), lecture fenetres visibles (`readAttachedWindowText`, `readUiAutomationTree`), `scanMemoryWindow`. |
| `write_freeze_core_manager.h/.cpp` | Ecriture memoire (`writeMemoryValue`, `writeMemoryValuesAtomic`, `writeMemoryValuesWithVariants`), rollback (`rollbackLastWrite`, `rollbackLastWriteBatch`), surveillance post-write. |
| `freeze_hotkey_overlay_manager.h/.cpp` | Freeze par polling (`setFreezeValue`, `setFreezeInterval`), hotkeys globales (`registerGlobalHotkey`...), overlay Trainer (`setTrainerOverlayVisible`, `updateTrainerOverlay`). |
| `debug_feature_manager.h/.cpp` | Breakpoints materiels/in-process (`findWhatWrites*`, `findWhatAccesses*`, `findWhatExecutes`, Page Guard, freeze par breakpoint `freezeWithBreakpoint`/`stopBreakpointFreeze`), speedhack, API hook, `disassembleBackward`. (`analyzeFieldStability` reste directement sur `application_controller.cpp`, pas dans ce manager.) |
| `code_patch_manager.h/.cpp` | AOB (`scanAobPattern`, `generateAobSignature`, `suggestCodePatches`), patch (`applyCodePatch`/`restoreCodePatch`), Auto-Assembler (`parseAutoAssemblerScript`/`executeAutoAssemblerScript`/`restoreAutoAssemblerScript`), hooks bas niveau. |
| `clr_inspector_bridge.h/.cpp` (classe `ClrInspectorBridge`) | Tout le pont CLR/.NET/ClrMD (`attachClrInspector`, `findClrObjects*`, `readClrObject`, `writeClrPrimitive*`, `callClrInstanceMethod`, GC roots, desassemblage). |
| `profile_manager.h/.cpp` | Profils `.keprofile` (CRUD, resolution), pointer chains (`scanPointerChains`, `resolvePointerChain`, `suggestStableLocatorForAddress`), pointer map (export/import/compare), pont Ghidra, patches/AutoAsm/Lua persistes en profil, `setProfileTargetDependencies`. |
| `kernel_driver_manager.h/.cpp` | Driver kernel : `probeKernelDriver`, `startKernelDriver`, `readMemoryKernel`, `writeMemoryKernel` (`writeMemoryValueKernel` reste sur `application_controller.cpp`). |
| `save_file_investigator.h/.cpp` (classe `SaveFileInvestigator`) | Fichiers de sauvegarde UWP : decouverte, lecture, LocalSettings, patch bytes, surveillance. |
| `settings_diagnostics_manager.h/.cpp` | `getSettings`/`saveSettings`, statut modele IA, chemins logs/debug/telemetrie, `getLogTail`, `exportDiagnostics`, stockage temporaire. |
| `smart_search_manager.h/.cpp` | Dispatch chat/IA (`startSmartSearch`), `startAutoResolve`, confirmations chat-memory, contexte Smart Search, rapport/memoire Auto Resolve, historique d'ecritures rejouable. |
| `automation_pipe_manager.h/.cpp` | Cycle de vie du mode Automation (`enable/disableAutomationMode`, `getAutomationPipeStatus`). |
| `investigation_notebook_manager.h/.cpp` (PHASE 120-E/F) | Carnet d'hypotheses : `addInvestigationHypothesis`, `recordInvestigationTestResult`, `getInvestigationNotebookSynthesis`, `resetInvestigationNotebook`. Delegue a `killai::InvestigationNotebook` (`ai/investigation_notebook.h/.cpp`) — moteur de ponderation deterministe pur, seul manager sans reference `ApplicationController&` (aucun etat process/memoire requis). |
| `auto_write_state_access.h/.cpp`, `scan_state_access.h/.cpp` | Facades d'acces internes (pas des "outils" au sens de cette carte) qui centralisent l'acces a `m_writeHistory`/`m_lastAutoWriteTargets` et `m_candidates`/`m_previousCandidates`/`m_snapshot` — consommees par plusieurs managers ci-dessus, jamais appelees directement depuis Vue. |
| `core/` | Moteurs deterministes : scanner, memoire, debug, patch, injection, profils, kernel bridge — inchange par ce refactor. |
| `ai/` | Planner IA local, tool registry, fallback deterministe, auto-resolver — inchange par ce refactor. |
| `tools/` | Helpers externes : driver kernel WDK, helper ClrMD .NET, handlers injectes. |

### Frontend — `ui/src/stores/`

| Fichier | Role |
| --- | --- |
| `app.ts` | Point d'entree public (`useAppStore()`) — reexporte (memes noms) l'etat/actions de tous les stores ci-dessous via `storeToRefs`. Garde en direct (non extrait) : pont Session→Trainer (`promoteSessionEntryToTrainer`...), domaine Watch adresses simples (`addAddressToWatch`, distinct du Pointer Chain Watch), etat Process/Memory/Diagnostics de base (`processName`, `memoryAccessMode`, `logFilePath`...), executeurs de checkpoint debug/patch (Find What Writes/disassemble/AOB/forcer valeur), glue bookmark↔write-target, panneaux Injection/Lua/AutoAsm. |
| `writeFreeze.ts` (S7) | Write/Freeze/Checkpoint : ecritures simples/multiples/atomiques, rollback, toggle freeze polling/BP, replay historique d'ecritures. |
| `trainer.ts` (S8) | Features Trainer : CRUD, apply/restore (resolution AOB/pointer-chain), hotkeys, overlay, sauvegarde profil, dependances. |
| `scanning.ts` (S11a) | Mecanique de scan pure (exact/next/unknown/groupe/chiffre) + pagination/selection candidats pour affichage. |
| `workspaceItems.ts` (S9a) | CRUD pur templates de structure + bookmarks workspace. |
| `workspaceSession.ts` (S9b) | Projets workspace (save/load/delete), export/import JSON+Markdown complet, panneau Pointer Chain Watch. |
| `assistantSmartSearch.ts` (S10+S11b) | Chat Assistant, Smart Search, Auto Resolve, promotion candidat → cible d'ecriture (`finalCandidateTargets`, `keepCandidate`/`ignoreCandidate`). |
| `clrInspector.ts` (S1) | Etat/actions CLR Inspector (miroir cote store du bridge C++ `ClrInspectorBridge`). |
| `speedhack.ts` (S2) | Speedhack, API hooking, blocage reseau. |
| `kernelDriver.ts` (S4) | Statut driver kernel, lecture/ecriture. |
| `automationPipe.ts` (S3) | Statut du pipe d'automatisation. |
| `settings.ts` (S12) | Parametres app, statut modele IA. |
| `actionLog.ts` (S5, fondation) | Journal d'actions (`addActionLog`), appele par ~tous les autres domaines. |
| `investigation.ts` (S6, fondation) | Timeline Investigation (`activeInvestigation`, `addInvestigationStep`), appele par ~tous les autres domaines. |
| `riskGate.ts` (fondation) | `confirmRiskAction` — seul mecanisme de confirmation avant une action a risque, dans toute l'app. |
| `trainerDependencies.ts` | Logique pure (resolution d'ordre/cycles de dependances Trainer), pas d'etat — consomme par `trainer.ts`. |
| `investigationNotebook.ts` (PHASE 120-F) | Carnet d'hypotheses : synthese confirmed/active/refuted, notes de preuve par hypothese, wrappers vers les 4 methodes backend. Independant de `investigation.ts` (timeline) — pas le meme systeme. |
| `tests/unit/`, `tests/integration/`, `tools/clr_inspector/KillEngineClrInspector.Tests/` | Tests C++/core/IA, tests runtime avec processus cible natif, tests end-to-end ClrMD/.NET — inchanges par ce refactor. |

## Processus, Attache, Modules

| Outil / Workflow | UI | Store | Bridge C++ | Core / Helper | Tests / Docs |
| --- | --- | --- | --- | --- | --- |
| Liste des processus | `ui/src/views/ProcessView.vue` | `app.ts`: `refreshProcesses` | `ApplicationController::getProcesses` (non delegue) | `core/process/process_enumerator.*` | `docs/USER_GUIDE.md`, `docs/PHASE_TRACKER.md` |
| Attacher / detacher un processus | `ProcessView.vue` | `app.ts`: `attach`, `detach` | `attachProcess`, `detachProcess` (non delegue) | `core/process/process_handle.*` | tests integration `KillEngineTestTarget` |
| Liste des modules | `ProcessView.vue` | `app.ts`: `refreshProcessModules` | `getProcessModules` (non delegue) | APIs Win32 module snapshot dans `ApplicationController` | `docs/USER_GUIDE.md` |
| Fichiers de sauvegarde UWP | `ExpertView.vue` (panneau "Fichiers de sauvegarde") | `app.ts`: `discoverSaveFiles`, `readSaveFileText` | `discoverProcessSaveFiles`, `readProcessSaveFileText` → `save_file_investigator.cpp` | `core/process/package_storage.*` | tracker PHASE 91/92 |
| LocalSettings UWP | `ExpertView.vue` + Assistant / pipe | `app.ts`: `inspectLocalSettings` | `inspectProcessLocalSettings` → `save_file_investigator.cpp` | `core/process/package_storage.*`: `inspectPackageLocalSettings` | `tests/unit/test_package_storage.cpp`, tracker PHASE 96/100 |
| Patch octets fichier sauvegarde | `ExpertView.vue` + Assistant / pipe | `app.ts`: `patchSelectedSaveFileBytes` | `patchProcessSaveFileBytes` → `save_file_investigator.cpp` | `core/process/package_storage.*`: `patchPackageSaveFileBytes` | `tests/unit/test_package_storage.cpp`, tracker PHASE 94/100 |
| Surveillance fichier sauvegarde | `ExpertView.vue` + Assistant / pipe | `app.ts`: `watchSelectedSaveFile`, `cancelSaveFileWatchAction` | `watchSaveFileForChanges`, `startSaveFileWatchAsync`, `cancelSaveFileWatch` → `save_file_investigator.cpp` | `core/process/file_watch.*` | `tests/unit/test_file_watch.cpp`, tracker PHASE 93/100 |
| Mode d'acces memoire Standard / Kernel | `ProcessView.vue` | `app.ts`: `memoryAccessMode`, `setMemoryAccessMode`, `kernelMemoryReady` | combine `probeKernelDriver` (→ `kernel_driver_manager.cpp`) et appels read/write kernel | `core/kernel/kernel_driver_bridge.*` | `docs/KILLENGINE_KERNEL_DRIVER_ARCHITECTURE.md` |
| Charger / tester le driver | `ProcessView.vue` | `kernelDriver.ts`: `refreshKernelDriverStatus`, `startKernelDriver` (reexporte par `app.ts`) | `probeKernelDriver`, `startKernelDriver` → `kernel_driver_manager.cpp` | service Windows `KillEngineKernel` + `KernelDriverBridge` | `docs/PHASE_TRACKER.md` PHASE 85 |

## Memoire, Carte, Preview, Hex

Toutes les methodes de cette section restent **directement dans `application_controller.cpp`** (jamais deleguees) — domaine basique/bas-risque, pas retenu par le refactor C1-C14.

| Outil / Workflow | UI | Store | Bridge C++ | Core / Helper | Tests / Docs |
| --- | --- | --- | --- | --- | --- |
| Carte memoire | `ui/src/views/MemoryView.vue`, `ExpertView.vue` | `app.ts`: `refreshMemoryMap` | `getMemoryMap` | `core/memory/memory_map.*`, `memory_region.*` | tests unitaires memory/scan |
| Preview / lecture bloc memoire | `MemoryView.vue`, `ExpertView.vue` | `app.ts`: `readMemoryPreview`, `readMemoryBlockForHexViewer` | `readMemoryPreview`, `readMemoryBlock` | `core/memory/memory_reader.*` | tracker |
| Edition hex | `MemoryView.vue` | `app.ts`: `writeMemoryHexForView` | `writeMemoryHex` | `core/memory/memory_writer.*` | `docs/USER_GUIDE.md` |
| Dump region memoire | `MemoryView.vue` / Expert | `app.ts` | `dumpMemoryRegion` | `MemoryReader`, filesystem Qt | tracker |
| Analyse structure autour d'une adresse | `MemoryView.vue`, `ExpertView.vue` | `app.ts`: actions structure | `analyzeStructureMemory` | `core/scanner/structure_analyzer.*` | `tests/unit` suite `StructureAnalyzer` |
| Delta d'offset entre deux instances | `ExpertView.vue` (boutons "Delta instances" / "Struct") | `app.ts` | `inferStructureInstanceDelta` (PHASE 239) | `core/scanner/structure_analyzer.*`: `killcore::inferStructureInstanceDelta` | `tests/unit` `StructureAnalyzer.InfersInstanceDelta*` |

## Scans Et Candidats

Bridge C++ delegue a `apps/desktop/scanning_core_manager.cpp` sauf mention contraire.

| Outil / Workflow | UI | Store | Bridge C++ | Core / Helper | Tests / Docs |
| --- | --- | --- | --- | --- | --- |
| Scan exact Assistant | `ui/src/views/AssistantView.vue` | `assistantSmartSearch.ts`: `startSmartSearch` (reexporte par `app.ts`) | `startSmartSearch` → `smart_search_manager.cpp`, puis `startExactScan` | `core/scanner/scan_engine.*`, `scan_types.*` | `tests/unit/test_ai_tools.cpp`, tests scan |
| Scan exact Expert | `ui/src/views/ExpertView.vue` | `scanning.ts`: `doExactScan` | `startExactScan`, `startExactScanAsync` | `ScanEngine`, `MemoryMap`, `MemoryReader` | `tests/unit` suites scanner |
| Scan multi-type / variantes | Assistant, Expert | `scanning.ts` | `startExactScanMultiType` | `core/scanner/value_variants.*`, `candidate_confidence.*` | `tests/unit` suites `ValueVariants` |
| Next scan | Assistant, Expert | `scanning.ts`: `doNextScan` | `nextScan`, `nextScanAsync` | `ScanEngine`, `CandidateStore` | tests scan/candidates |
| Unknown initial value | Assistant, Expert | `scanning.ts`: `captureUnknownSnapshot`, `doUnknownNextScan` | `captureUnknownSnapshot`, `unknownNextScan`, variantes async | `core/snapshot/*`, `ScanEngine`, LZ4 | tracker, tests unknown |
| Scan chiffre / obfusque | Assistant, Expert | `scanning.ts`: `doEncryptedScan` | `scanEncryptedValue` | `core/scanner/encrypted_scan.*` | `tests/unit` suite `EncryptedScan` |
| Group scan | Expert | `scanning.ts`: `doGroupScan` | `scanGroupScan` | `encrypted_scan` / variantes scan groupe | tracker |
| CandidateStore, pagination, undo | Expert, Assistant | `scanning.ts`: pages, selection, `undoCandidateScan` | `getCandidates`, `undoCandidateScan`, `cancelActiveScan` | `core/candidates/candidate_store.*` | `tests/unit` suite `CandidateStoreTest` |
| Promotion candidat → cible d'ecriture | Expert, Assistant | `assistantSmartSearch.ts`: `keepCandidate`, `ignoreCandidate`, `finalCandidateTargets` | — (etat frontend uniquement) | — | tracker S11b |
| Watch live candidats/adresses | Expert | `app.ts`: `addAddressToWatch` (Watch adresses simples, distinct du Pointer Chain Watch) | `readMemoryPreview` (non delegue, reste dans `application_controller.cpp`), timers UI | `MemoryReader` | tracker |

## Ecriture, Rollback, Freeze

Bridge C++ delegue a `write_freeze_core_manager.cpp` (ecritures/rollback), `freeze_hotkey_overlay_manager.cpp` (freeze polling/hotkeys/overlay) ou `debug_feature_manager.cpp` (freeze par breakpoint) — precise par ligne.

| Outil / Workflow | UI | Store | Bridge C++ | Core / Helper | Tests / Docs |
| --- | --- | --- | --- | --- | --- |
| Ecriture valeur typee | Assistant, Memory, Expert | `writeFreeze.ts`: `writeSelectedValue`, `executeCheckpointWrite` | `writeMemoryValue` → `write_freeze_core_manager.cpp` | `core/memory/memory_writer.*`, `value_variants.*` | tests integration write/rollback |
| Ecriture multi-adresses | Expert | `writeFreeze.ts`: `writeSelectedAddresses`, `writeSelectedTargets` | `writeMemoryValuesWithVariants` → `write_freeze_core_manager.cpp` | `MemoryWriter`, `value_variants` | tracker Trace UI |
| Ecriture atomique suspend threads | Expert, CLR | `writeFreeze.ts`: `writeSelectedAtomic` | `writeMemoryValuesAtomic` → `write_freeze_core_manager.cpp` | thread suspend guard | tracker |
| Rollback derniere ecriture / batch | Assistant, Expert | `writeFreeze.ts`: `rollbackLastWrite`, `rollbackLastWriteBatch` | memes noms → `write_freeze_core_manager.cpp` | historique dans `AutoWriteStateAccess` | tests integration |
| Freeze polling | Expert | `writeFreeze.ts`: `toggleFreeze`, `freezeCandidateCurrent` | `setFreezeValue`, `setFreezeInterval` → `freeze_hotkey_overlay_manager.cpp` | `core/freeze/freeze_manager.*` | `tests/unit` suite `FreezeManager` |
| Detection freeze instable | Assistant, Expert | `app.ts`: signal `freezeInstabilityDetected` | signal Qt (emis depuis `freeze_hotkey_overlay_manager.cpp`) | `FreezeManager::recordPollTick` | `tests/unit` suite `FreezeManager` |
| Freeze hardware breakpoint | Expert | `writeFreeze.ts`: `startBreakpointFreeze`, `escalateFreezeToBreakpoint` | `freezeWithBreakpoint`, `stopBreakpointFreeze`, `getBreakpointFreezeStats` → `debug_feature_manager.cpp` | `core/debug/breakpoint_freeze.*`, `hardware_breakpoint.*` | integration `PowerUpRuntimeTest.BreakpointFreeze*` |
| Freeze in-process breakpoint | Expert | `backend.ts`: in-process freeze methods | `startInProcessBreakpointFreeze` → `debug_feature_manager.cpp` | `core/debug/inprocess_breakpoint.*` + handler DLL | integration / tracker |

## Trace UI String Et Investigation De Valeurs Affichees

Bridge C++ delegue a `apps/desktop/display_string_investigator.cpp` (classe `UiStringInvestigator`), sauf mention contraire.

| Outil / Workflow | UI | Store | Bridge C++ | Core / Helper | Tests / Docs |
| --- | --- | --- | --- | --- | --- |
| Scanner texte affiche ASCII/UTF-16 | `ExpertView.vue`, Assistant | `app.ts`: `scanUiStrings` | `scanUiStrings` | `core/scanner/display_value_tracker.*` | `tests/unit` suite `UiStringTracker` |
| Filtrer strings apres variation | Expert | `app.ts`: `trackUiStringCandidates` | `trackUiStringCandidates` | `display_value_tracker.*` | `docs/PHASE_TRACKER.md` PHASE 77 |
| Analyser sources numeriques proches | Expert | `app.ts`: `analyzeUiStringSources` | `analyzeUiStringSources` | `display_value_tracker.*`, `value_variants.*` | tests `UiStringTracker` |
| Tracker sources numeriques | Expert | `app.ts`: `trackUiStringSources` | `trackUiStringSources` | `display_value_tracker.*` | tracker |
| Origines / backrefs de strings | Expert | `app.ts`: `inspectUiStringOrigins` | `inspectUiStringOrigins` | scan pointeurs via `MemoryReader` | tracker |
| Investigation snapshots autour strings/sources | Expert | `app.ts`: `startUiStringInvestigation`, `finishUiStringInvestigation` | memes noms | `MemoryReader`, hashes blocs | tracker PHASE 77 |
| Diff pages modifiees | Assistant mode Inspecteur, pipe | `assistantSmartSearch.ts` | `startChangedPagesDiff`, `finishChangedPagesDiff` | `MemoryMap`, `MemoryReader`, `value_variants.*` | tracker PHASE 82/83 |
| Lecture texte fenetres visibles | Assistant / pipe | `assistantSmartSearch.ts` | `readAttachedWindowText`, `readUiAutomationTree` (non delegues, restent sur `application_controller.cpp`) | Win32 `EnumWindows` / UI Automation | tracker PHASE 82 |

## Debug, Breakpoints, Page Guard

Bridge C++ delegue a `apps/desktop/debug_feature_manager.cpp`, sauf `testCandidateFieldsAsync` (reste dans `application_controller.cpp`) et `disassembleBackward` qui vit aussi dans `debug_feature_manager.cpp`.

| Outil / Workflow | UI | Store | Bridge C++ | Core / Helper | Tests / Docs |
| --- | --- | --- | --- | --- | --- |
| Find What Writes | Expert, Assistant checkpoints | `app.ts` | `findWhatWrites`, `findWhatWritesAsync`, signal | `core/debug/hardware_breakpoint.*`, `breakpoint_arbiter.*` | integration / tracker PHASE 17 |
| Find What Accesses | Expert | `backend.ts` | `findWhatAccessesAsync`, signal | `hardware_breakpoint.*` | tracker |
| Find What Executes | Expert / pipe | `backend.ts` | `findWhatExecutes` | `hardware_breakpoint.*` | tracker PHASE 78 |
| Page Guard watch | Expert | `backend.ts` | `startPageGuardWatchAsync`, cancel, signal | `core/debug/page_guard.*`, `page_guard_handler/*` | integration `PageGuardCapturesRemoteStressRewrite` |
| Breakpoint in-process watch | Expert / pipe | `backend.ts` | `startInProcessBreakpointWatchAsync`, `startInProcessExecuteWatchAsync` | `core/debug/inprocess_breakpoint.*`, handler DLL | tracker PHASE 75/82 |
| Desassemblage en amont | Expert | `app.ts`: `disassembleBackward` | `disassembleBackward` | `core/patch/instruction_patch_suggester.*` | `tests/unit` suite `InstructionPatchSuggester` |
| Test automatique champs candidats | Expert | `app.ts`: `testCandidateFieldsAsync` | `testCandidateFieldsAsync` (non delegue, reste sur `application_controller.cpp`) | `instruction_patch_suggester.*`, `MemoryWriter` | tracker XP/Solitaire |
| Champ affiche vs champ source | Expert / Assistant | `app.ts` | `analyzeFieldStability` (non delegue, reste sur `application_controller.cpp`) | `core/scanner/display_source_classifier.*` | `tests/unit` suite `DisplaySourceClassifier` |

## AOB, Patch, Auto-Assembler

Bridge C++ delegue a `apps/desktop/code_patch_manager.cpp`.

| Outil / Workflow | UI | Store | Bridge C++ | Core / Helper | Tests / Docs |
| --- | --- | --- | --- | --- | --- |
| Scan AOB | Expert, Trainer | `app.ts` | `scanAobPattern` | `core/patch/aob_scanner.*` | `tests/unit` suite `AobScanner` |
| Generation signature AOB | Expert depuis hit/debug | `app.ts`: `generateAobSignature` | `generateAobSignature` | `aob_scanner.*`, `MemoryReader` | tests AOB |
| Suggestion patch code | Expert | `app.ts`: `suggestCodePatches` | `suggestCodePatches` | `core/patch/instruction_patch_suggester.*`, Zydis si dispo | `tests/unit` suite `InstructionPatchSuggester` |
| Appliquer / restaurer patch bytes | Expert, Trainer | `app.ts`: `writeFreeze.ts`/`trainer.ts` selon origine | `applyCodePatch`, `restoreCodePatch` | `core/patch/code_patch.*`, `MemoryWriter` | tests `CodePatch`, `ProfilePatchState` |
| Auto-Assembler parse/execute/restore | Expert / Scripting | `app.ts` | `parseAutoAssemblerScript`, `executeAutoAssemblerScript`, `restoreAutoAssemblerScript` | `code_patch_manager.cpp` (parser/executor) | `tests/unit` suite `AutoAssembler` |
| Force value hook sur instruction | Expert | `app.ts`: `executeCheckpointForceValue` | `forceWriteInstructionValue` (non delegue, reste sur `application_controller.cpp`) | shellcode/trampoline + `MemoryWriter` | tracker |

### Chemin Mental Pour Les Patchs De Code

Les patchs de code servent quand la valeur en jeu n'est pas stable a une adresse simple, ou quand le jeu recalcule/re-ecrit trop vite. Au lieu de modifier seulement une adresse de donnees, KillEngine cherche l'instruction qui manipule la valeur, puis propose une modification du code execute.

Flux habituel :

1. Trouver une adresse candidate ou une instruction interessante avec scan, Trace UI string, Find What Writes/Accesses/Executes, Page Guard ou une signature AOB.
2. Utiliser `suggestCodePatches(address, { maxBytes: 16 })` pour lire/desassembler une petite fenetre autour de l'adresse et proposer des patchs prudents, par exemple NOP, jump court, ou patch adapte a l'instruction.
3. Appliquer les octets avec `applyCodePatch(address, bytes, { verify: true })`. Le backend sauvegarde les octets originaux pour pouvoir restaurer.
4. Restaurer avec `restoreCodePatch(address)` si le patch casse le comportement, ou sauvegarder dans un profil via les outils Profile/Trainer si le patch est bon.
5. Si le but est de forcer une valeur depuis une instruction qui ecrit, regarder `forceWriteInstructionValue(...)` : cet outil construit un hook/trampoline pour imposer la valeur au moment ou l'instruction s'execute.

Fichiers a ouvrir en premier pour ce sujet :

| Besoin | Fichiers |
| --- | --- |
| Voir les boutons et le workflow Expert | `ui/src/views/ExpertView.vue`, autour de `suggestCodePatches`, `applyCodePatch`, `restoreCodePatch`, `forceWriteInstructionValue` |
| Voir le contrat TypeScript | `ui/src/services/backend.ts`, interfaces `CodePatchResult`, `CodePatchSuggestionResult`, methodes `applyCodePatch`, `suggestCodePatches`, `restoreCodePatch`, `forceWriteInstructionValue` |
| Voir les actions reutilisees par profils/trainer/assistant | `ui/src/stores/writeFreeze.ts` et `ui/src/stores/trainer.ts` (plus `app.ts` pour les checkpoints debug/patch restes generiques) |
| Voir le pont natif | `apps/desktop/application_controller.h` (signatures) + `apps/desktop/code_patch_manager.h/.cpp` (implementation reelle) |
| Voir la pose/restauration d'octets | `core/patch/code_patch.h/.cpp` |
| Voir l'analyse d'instructions et suggestions | `core/patch/instruction_patch_suggester.h/.cpp` |
| Voir les signatures AOB | `core/patch/aob_scanner.h/.cpp` |
| Voir les preuves | `tests/unit/test_aob_scanner.cpp`, tests `CodePatch`, `ProfilePatchState`, `AutoAssembler` |

## Injection, Hooks, Speedhack, Reseau

Bridge C++ delegue a `code_patch_manager.cpp` (DLL/hooks) ou `debug_feature_manager.cpp` (speedhack, API hook) ; injection DLL et reseau restent directement dans `application_controller.cpp`.

| Outil / Workflow | UI | Store | Bridge C++ | Core / Helper | Tests / Docs |
| --- | --- | --- | --- | --- | --- |
| Injection DLL | Expert `InjectionPanel.vue` | `app.ts`: `injectDll` | `injectDllIntoProcess` (non delegue) | `core/inject/dll_injector.*` | integration injection |
| Function hook bas niveau | Expert `InjectionPanel.vue` | `backend.ts`: `installFunctionHook`, `removeFunctionHook` | memes noms → `code_patch_manager.cpp` | `core/inject/function_hook.*` | integration helpers |
| API Hook MinHook | Expert `InjectionPanel.vue` | `app.ts` | `startApiHook`, `stopApiHook`, `getApiHookStatus` → `debug_feature_manager.cpp` | `core/inject/api_hook.*`, `api_hook_handler/*`, MinHook | integration `ApiHookCountsRealCallsOutOfProcess` |
| Speedhack | `ui/src/views/SpeedhackView.vue` | `speedhack.ts` | `startSpeedhack`, `setSpeedhackFactor`, `stopSpeedhack`, status → `debug_feature_manager.cpp` | `core/debug/speedhack.*`, `speedhack_clock.*`, handler DLL | `tests/unit` suite `SpeedhackClock` |
| Blocage reseau processus | `ui/src/views/NetworkView.vue` | `speedhack.ts` | `blockProcessNetwork`, `unblockProcessNetwork`, `getProcessNetworkBlockStatus` (non delegue) | PowerShell `NetSecurity` via `ShellExecuteExW runas` | tracker PHASE 84 |
| Runtime Integrity Probe | pas forcement UI directe | scripts / injection selon usage | cible DLL build | `tools/runtime_integrity_probe/*` | tracker PHASE 72 |

## Kernel Driver

Bridge C++ delegue a `apps/desktop/kernel_driver_manager.cpp`, sauf `writeMemoryValueKernel` (reste dans `application_controller.cpp`).

| Outil / Workflow | UI | Store | Bridge C++ | Core / Helper | Tests / Docs |
| --- | --- | --- | --- | --- | --- |
| Build driver | terminal | script | n/a | `tools/kernel_driver/KillEngineKernel/*.cpp`, `.vcxproj` | `scripts/build-kernel-driver.ps1` |
| Installer / demarrer service kernel | Settings, ProcessView Kernel | `kernelDriver.ts`: `startKernelDriver` | `startKernelDriver` | service Windows `KillEngineKernel`, `scripts/install-kernel-driver.ps1` | `docs/KILLENGINE_KERNEL_DRIVER_ARCHITECTURE.md` |
| Probe driver | Settings, ProcessView Kernel | `kernelDriver.ts`: `refreshKernelDriverStatus` | `probeKernelDriver` | `core/kernel/kernel_driver_bridge.*`, IOCTL HealthProbe | `tests/unit` suite `KernelDriverBridge` |
| Lecture memoire kernel | Settings, Memory, Expert | `kernelDriver.ts`: `readMemoryKernel` | `readMemoryKernel` | `KernelDriverBridge::readMemory`, driver `driver.cpp` | tracker PHASE 39 |
| Ecriture memoire kernel | Settings, Expert, Assistant explicite | `app.ts`: `writeMemoryKernel`; `writeFreeze.ts`: `executeCheckpointKernelWrite` | `writeMemoryKernel` → `kernel_driver_manager.cpp` ; `writeMemoryValueKernel` non delegue | `KernelDriverBridge::writeMemory`, driver `driver.cpp` | tracker PHASE 39/42 |

## Pointer Chains Et Profils

Bridge C++ delegue a `apps/desktop/profile_manager.cpp` pour tout ce cluster (profils, pointer chains, Ghidra, pointer map, patches/scripts persistes en profil).

| Outil / Workflow | UI | Store | Bridge C++ | Core / Helper | Tests / Docs |
| --- | --- | --- | --- | --- | --- |
| Scan pointer chains | Expert | `app.ts` | `scanPointerChains` | `core/pointer/pointer_scanner.*`, `pointer_chain.*` | `tests/unit` suite `PointerChain` |
| Resolution pointer chain | Expert, Profile | `workspaceSession.ts` (Pointer Chain Watch) | `resolvePointerChain` | `PointerChain`, `MemoryReader` | tests PointerChain |
| Suggest stable locator | Assistant/Expert apres ecriture | `writeFreeze.ts`: `autoSuggestStableLocatorIfWorthwhile` | `suggestStableLocatorForAddress` | `PointerScanner`, profile locators | tracker |
| Profils `.keprofile` | `ui/src/views/ProfileView.vue`, Trainer | `trainer.ts`: `saveTrainerFeatureToProfile` | `saveProfileTarget`, `listProfiles`, `loadProfile`, `resolveProfileTarget`, etc. | `core/profiles/profile_store.*`, `locator.*` | `tests/unit` suite `PointerChain` profile round trips ; `scripts/test-automation-pipe-profile-methods.ps1` |
| Pointer map partageable | Profile | `backend.ts`: `exportPointerMap`, `importPointerMap` | memes noms | `ProfileStore::exportPointerMap`, `ProfileStore::mergePointerMap` | `tests/unit/test_profile_store.cpp` |
| Pont Ghidra | Profile | `backend.ts`: `exportGhidraArtifacts`, `importGhidraSymbols` | memes noms | `core/profiles/ghidra_bridge.*`, `profile_store.*` | `tests/unit/test_ghidra_bridge.cpp` |
| Patches/scripts dans profils | Profile, Trainer | `trainer.ts`: `saveTrainerFeatureToProfile` | `saveProfileCodePatch`, `applyProfileCodePatch`, `saveProfileAutoAsmScript`, etc. | `profile_store.*`, `profile_patch_state.*`, AOB/Patch core | `tests/unit` suite `ProfilePatchState` |

## Trainer, Hotkeys, Overlay

| Outil / Workflow | UI | Store | Bridge C++ | Core / Helper | Tests / Docs |
| --- | --- | --- | --- | --- | --- |
| Features Trainer locales | `ui/src/views/TrainerView.vue`, `ProfileView.vue`, Expert | `trainer.ts`: `trainerFeatures`, apply/restore/export | profils + write/patch methods (voir sections dediees) | `ProfileStore`, `AOB`, `MemoryWriter` | `docs/USER_GUIDE.md` |
| Dependances Trainer | Trainer, Profile | `trainer.ts`: `updateTrainerFeatureDependencies` | `setProfileTargetDependencies` → `profile_manager.cpp`, `loadProfile` | `ProfileTarget::dependsOn` dans `.keprofile` | `tests/unit/test_profile_store.cpp` |
| Hotkeys globales | Trainer/Settings | `trainer.ts`: `registerTrainerFeatureHotkey` | `registerGlobalHotkey`, `unregisterGlobalHotkey` → `freeze_hotkey_overlay_manager.cpp`, signal `globalHotkeyTriggered` | native event filter dans desktop | `tests/unit` suite `GlobalHotkey` |
| Overlay Trainer | Trainer | `trainer.ts`: `setTrainerOverlay`, `refreshTrainerOverlay` | `setTrainerOverlayVisible`, `updateTrainerOverlay` → `freeze_hotkey_overlay_manager.cpp` | Qt overlay widgets | tracker |
| Promotion Session → Trainer | Trainer (via panneau Write/Freeze) | `app.ts`: `promoteSessionEntryToTrainer`, `promoteSessionGroupToTrainer` (pont Session, appelle `trainer.ts::createTrainerFeature`) | — (orchestration frontend) | — | tracker |

## IA, Assistant, Auto Resolve

Bridge C++ delegue a `apps/desktop/smart_search_manager.cpp` — le cluster le plus branche de tout `ApplicationController` (`startSmartSearch` seul depasse 20 branches), voir `docs/AUTOMATION_API_REFERENCE.md` section "Chat / Smart Search / Auto Resolve" pour le detail des formes de reponse.

| Outil / Workflow | UI | Store | Bridge C++ | Core / Helper | Tests / Docs |
| --- | --- | --- | --- | --- | --- |
| Assistant chat / Smart Search | `ui/src/views/AssistantView.vue` | `assistantSmartSearch.ts`: `startSmartSearch`, messages, recovery actions | `startSmartSearch` | `ai/ai_engine.*`, `intent_contract.*`, `tool_registry.*`, `llama_runtime.*` | `tests/unit/test_ai_tools.cpp`, `test_ai_adaptive.cpp` |
| Mode Inspecteur Codex | Assistant | `assistantSmartSearch.ts` | `startSmartSearch` dispatch vers outils lecture seule | `ai/llama_runtime.cpp` prompt, `ai/ai_engine.cpp` fallback | tracker PHASE 83 |
| Auto Resolve | Assistant, Investigation | `assistantSmartSearch.ts`: `doAutoResolve`, `autoResolveReport` | `startAutoResolve`, `getAutoResolveReport` | `ai/auto_resolver.*`, telemetry JSONL | `tests/unit` suite `AutoResolverTest` |
| Tool registry IA | pas UI directe | `backend.ts`: tool calls result | `startSmartSearch` dispatch | `ai/tool_registry.*`, `tool_validator.*` | `tests/unit` `AIToolRegistryTest` |
| Llama/Qwen local | Settings model panel | `settings.ts`: `aiModelStatus`, `refreshAiModelStatus` | `getAiModelStatus`, `browseForModelFile` → `settings_diagnostics_manager.cpp` ; `saveSettings` idem | `ai/llama_runtime.*`, `llama_server.*`, `model_locator.*`, `model/*` | `model/README.md` |
| Automation pipe | scripts | `automationPipe.ts`: statut ; `scripts/automation-pipe-call.ps1` | toutes methodes `Q_INVOKABLE` par reflexion ; cycle de vie via `automation_pipe_manager.cpp` | `apps/desktop/automation_pipe_server.h/.cpp` | `docs/AUTOMATION_API.md`, `scripts/test-automation-pipe-*.ps1` |
| Carnet d'hypotheses (PHASE 120-E/F) | `InvestigationView.vue` (nouvelle section, distincte de la timeline existante) | `investigationNotebook.ts` : `addHypothesis`, `recordTestResult`, `refreshNotebook`, `resetNotebook` | `addInvestigationHypothesis`, `recordInvestigationTestResult`, `getInvestigationNotebookSynthesis`, `resetInvestigationNotebook` → `investigation_notebook_manager.cpp` | `ai/investigation_notebook.h/.cpp` (`killai::InvestigationNotebook`, moteur de ponderation deterministe, sans generation d'hypothese ni de "prochaine experience" — role reserve au modele local, PHASE 120-G non livree) | `tests/unit/test_investigation_notebook.cpp`, `docs/AUTOMATION_API_REFERENCE.md` section "Carnet d'hypotheses", `docs/PHASE_TRACKER.md` PHASE 120-E/F, `diagnostics/phase120f-investigation-notebook.png` |

## CLR / .NET / ClrMD

Bridge C++ delegue a `apps/desktop/clr_inspector_bridge.cpp` (classe `ClrInspectorBridge`) pour tout ce cluster.

| Outil / Workflow | UI | Store | Bridge C++ | Core / Helper | Tests / Docs |
| --- | --- | --- | --- | --- | --- |
| Attacher helper CLR | `ui/src/views/ClrInspectorView.vue` | `clrInspector.ts`: `attachClrInspector` | `attachClrInspector` (seul expose en `Q_INVOKABLE` ; `ensureClrInspectorStarted`/`callClrInspectorRpc` sont des methodes internes de `ClrInspectorBridge`, pas exposees directement, utilisees par toutes les methodes de ce cluster) | `tools/clr_inspector/KillEngineClrInspector/*` | `docs/KILLENGINE_CLR_INSPECTOR_SPEC.md` |
| Trouver objets par type/champ | `ClrInspectorView.vue` | `clrInspector.ts` | `findClrObjectsByType`, `findClrObjectsByFieldValue` | `ClrSession.cs`, `MethodDispatcher.cs` | `EndToEndTests.cs` |
| Lire objet manage | `ClrInspectorView.vue` | `clrInspector.ts`: `readClrObject` | `readClrObject` | `ClrSession.DescribeObject`, collections BCL (y compris concurrentes depuis PHASE 80/202/222/225) | tests ClrMD |
| Ecrire champ/chemin CLR | `ClrInspectorView.vue` | `clrInspector.ts` / `app.ts` (7 fonctions a risque, `confirmRiskAction` garde ici) | `writeClrPrimitiveField`, `writeClrPrimitivePath`, batch/atomic/locator variants | `ClrSession.WritePrimitivePath*` | tests ClrMD |
| Appeler setter C# reel | `ClrInspectorView.vue` | `app.ts`: `callClrInstanceMethod` | `callClrInstanceMethod` | ClrMD resolve + shellcode natif, `NativeSetterInvoker` test | `EndToEndTests.cs` |
| GC roots / chemin root | `ClrInspectorView.vue` | `clrInspector.ts` | `enumerateClrRoots`, `findClrGcRootPath` | `ClrSession.FindGcRootPath` (BFS multi-source, PHASE finale) | tests ClrMD |
| Rapport objet | `ClrInspectorView.vue` | `clrInspector.ts`: `generateClrObjectReport` | `generateClrObjectReport` | `ClrSession.GenerateObjectReport` | tests ClrMD |
| Desassemblage methode CLR | `ClrInspectorView.vue` | `clrInspector.ts`: `disassembleClrMethod` | `disassembleClrMethod` | `ClrSession.ResolveInstanceMethodAddress`, `instruction_patch_suggester.*` | tests C++ + ClrMD docs |
| Cible CLR de test | pas UI | scripts build/test | n/a | `tests/clr_targets/KillEngineClrTestTarget/*` | `docs/KILLENGINE_CLR_TEST_TARGET_SPEC.md` |

## Scripting

| Outil / Workflow | UI | Store | Bridge C++ | Core / Helper | Tests / Docs |
| --- | --- | --- | --- | --- | --- |
| Vue scripting Lua | `ui/src/views/ScriptingView.vue` | `app.ts`: scripting actions | `getLuaScriptingStatus`, `executeLuaScript(Async)` (non delegue, reste sur `application_controller.cpp`) | `scripts/killengine.lua` (wrapper `ke.*`, decodeur JSON maison avec support `\uXXXX` depuis PHASE 240), `scripts/automation-pipe-call.ps1` | `scripts/test-lua-examples.ps1`, `docs/AUTOMATION_API.md` |
| Auto-Assembler CE-like | Expert / Scripting | voir section AOB/Patch | voir section AOB/Patch (`code_patch_manager.cpp`) | parser/executor auto-asm | `tests/unit` suite `AutoAssembler` |

## Diagnostic, Logs, Packaging

Bridge C++ delegue a `apps/desktop/settings_diagnostics_manager.cpp`, sauf mention contraire.

| Outil / Workflow | UI | Store | Bridge C++ | Core / Helper | Tests / Docs |
| --- | --- | --- | --- | --- | --- |
| Logs app | Settings | `app.ts`: log actions | `getLogTail`, `getLogFilePath` | `core/logging/logger.*` | `tests/unit` suite `LoggerTest` |
| Telemetry scan / smart search | Settings / diagnostics | `backend.ts`: paths/events | `getSmartSearchDebugEvents`, `getScanTelemetryFilePath` | JSONL sous `%LOCALAPPDATA%` | tracker |
| Export diagnostics | Settings | `app.ts`: export diagnostics | `exportDiagnostics` | zip/manifest | `docs/USER_GUIDE.md` |
| Nettoyage stockage temporaire | Settings | `app.ts`: temp storage actions | `getTemporaryStorageStatus`, `clearTemporaryStorage` | `CandidateStore`, snapshots | tests CandidateStore |
| Build app | terminal | n/a | n/a | `scripts/build.ps1`, CMake/Ninja | `README.md` |
| Build UI | terminal | n/a | n/a | `scripts/build_ui.ps1`, Vite | `ui/package.json` |
| Package Windows | terminal | n/a | n/a | `packaging/`, `scripts/package-windows.ps1` (embarque desormais `docs/AUTOMATION_API.md`, PHASE 233) | `packaging/README.md` |

## Regle Pour Ajouter Un Nouvel Outil

1. Ajouter la logique core/helper si necessaire.
2. Exposer une methode dans `apps/desktop/application_controller.h` — decider si l'implementation va directement dans `application_controller.cpp` (outil simple/isole) ou dans un manager existant/nouveau (voir table "Fichiers Centraux" ci-dessus pour choisir le bon domaine, meme patron `friend class XxxManager` + `std::unique_ptr<XxxManager> m_xxxManager` que les managers deja extraits).
3. Ajouter le contrat dans `ui/src/services/backend.ts`, y compris le mock dev.
4. Ajouter l'action dans le store frontend concerne (voir table "Fichiers Centraux" cote `ui/src/stores/` — pas forcement `app.ts`) ; si `app.ts` doit continuer a l'exposer publiquement, ajouter juste une reexportation `storeToRefs`/destructure, pas de logique dupliquee.
5. Ajouter le bouton/panneau dans la vue Vue concernee.
6. Si l'Assistant doit choisir l'outil, ajouter `ai/tool_registry.cpp`, le prompt/fallback si utile, puis le dispatch dans `startSmartSearch` (`smart_search_manager.cpp`).
7. Ajouter tests selon le risque.
8. Documenter la forme de reponse dans `docs/AUTOMATION_API_REFERENCE.md` si la methode est nouvelle (ou re-verifier l'entree existante si elle a change) — voir la convention `Select-String -Pattern 'result\["\w+"\]\s*='` dans `docs/AUTOMATION_API.md` si le doc n'a pas encore ete mis a jour.
9. Documenter dans `docs/PHASE_TRACKER.md` avec quoi/pourquoi/comment verifie.

## Notes Importantes

- `application_controller.cpp` reste un fichier tres partage (table des matieres + methodes non deleguees) : toujours relire juste avant d'editer, meme s'il ne porte plus toute la logique.
- `ui/src/stores/app.ts` reste le point d'entree public de tous les stores frontend : toute nouvelle lecture externe (vue, composable) continue de passer par `useAppStore()`, meme si l'etat reel vit dans un store dedie.
- `ui/src/services/backend.ts` et `ui/src/views/ExpertView.vue` restent des fichiers tres partages : toujours relire juste avant d'editer.
- Pour une methode backend appelee par Vue, verifier les deux cotes : `Q_INVOKABLE` C++ (et son manager reel si delegue) et interface/mock TypeScript.
- Pour un outil a risque (`write`, `debug`, `patch`, `injection`, `kernel`), chercher `confirmRiskAction` dans `ui/src/stores/riskGate.ts` (le store qui le possede desormais) — c'est le seul mecanisme de confirmation autorise dans toute l'app.
- Pour connaitre la forme exacte de la reponse JSON d'une methode `Q_INVOKABLE`, voir `docs/AUTOMATION_API_REFERENCE.md` (PHASE 238) ou, si la methode a change depuis, la convention de recherche dans `docs/AUTOMATION_API.md`.
- Pour comprendre si une capacite est vraiment validee, lire l'entree correspondante dans `docs/PHASE_TRACKER.md` (ou `docs/PHASE_TRACKER_HISTORY.md` si elle a ete archivee), pas seulement le code.
