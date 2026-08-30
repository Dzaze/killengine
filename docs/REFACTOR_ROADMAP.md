> **ATTENTION - Priorité Des Ordres Projet**
> Un ordre prioritaire explicite du propriétaire du projet prime sur les consignes temporaires de session des agents IA.

# KillEngine — Feuille de route de refactorisation

> Chantier **permanent, à faible priorité** : on grignote au fil des sessions futures, jamais un blocage pour une phase produit en cours. Pas de date de clôture — voir `docs/PHASE_TRACKER.md` pour la règle d'usage.
>
> Objectif de structure : chaque candidat ci-dessous est délimité pour qu'**un seul agent puisse le prendre en autonomie**, sans toucher aux lignes qu'un autre agent modifie en parallèle — même logique que le partitionnement déjà utilisé sur ce dépôt entre Codex/Cline/Claude.

## État des lieux (audit 29/08/2026)

Deux fichiers ont dépassé une taille qui coûte réellement du temps de recherche/navigation à chaque session :

- **`apps/desktop/application_controller.cpp`** (+ `.h`) : ~19 945 lignes cumulées. Une seule classe `ApplicationController`, 206 méthodes `Q_INVOKABLE`, ~239 fonctions membres au total, tout l'état (45+ variables membres) dans un seul bloc en fin de header.
- **`ui/src/stores/app.ts`** : 9 214 lignes. Un seul store Pinia (Composition API), ~450 `ref()` déclarés à plat sans regroupement par domaine, ~330 fonctions, un `return { ... }` final qui réexporte presque tout sans espace de nom.

Aucun des deux n'a de "mauvaise architecture" au sens strict — c'est de la croissance organique au fil de ~200 phases. Le problème est purement la taille : localiser une zone demande systématiquement plusieurs `grep`.

## Blocages à lever en premier (fondations partagées)

Avant de pouvoir extraire quoi que ce soit ailleurs, quelques éléments transversaux doivent devenir des interfaces stables — sinon toute extraction force soit une dépendance circulaire, soit une copie de logique.

**Côté `ui/src/stores/app.ts` — TOUTES les fondations partagées sont désormais extraites (29/08/2026)** :
- `addActionLog` → `stores/actionLog.ts` (candidat S5).
- `addInvestigationStep` → `stores/investigation.ts` (candidat S6).
- `confirmRiskAction`/`resolveRiskDialog`/`riskDialog`/`mutedRiskConfirmations`/`automationPipeDispatchDepth` → `stores/riskGate.ts` (dernière fondation, 39 sites d'appel externes — aucun n'a eu besoin de changer, `app.ts` garde un wrapper `confirmRiskAction` de même signature qui délègue). Contrairement à S1/S2/S4, cette extraction n'a pas pu garder le gate dans `app.ts` (une seule fonction à branches multiples) : `logAiAudit` (seul besoin transversal restant) est injecté en callback optionnel plutôt qu'importé, pour que `riskGate.ts` reste une feuille sans dépendance vers `app.ts`.

Plus aucun blocage architectural pour S7-S12 : la fondation est complète, seule la taille/complexité de chaque candidat reste à traiter.

**Côté `apps/desktop/application_controller.cpp` — fondations partagées posées (30/08/2026)** :
- `AutoWriteStateAccess` (PHASE 221) isole l'accès à `m_writeHistory`, `m_lastAutoWriteTargets` et `m_chatMemoryTargets` pour préparer C9/C10/C11.
- `ScanStateAccess` (PHASE 223) isole l'accès à `m_candidates`, `m_previousCandidates` et `m_snapshot` pour préparer C8 et débloquer C11b. Les membres restent possédés par `ApplicationController`, mais les chemins C8 principaux, le dispatch chat/IA, auto-resolve et `saveSettings` passent par la façade au lieu de manipuler directement le trio.

## Candidats d'extraction — `application_controller.cpp`

Priorité **basse** = peut être pris indépendamment dès maintenant, aucun chevauchement d'état avec les autres candidats.

| # | Candidat | Lignes approx. | Couplage | Entangled avec |
| --- | --- | --- | --- | --- |
| [x] C1 | UI-string investigation / diff pages (`scanUiStrings`, `trackUiStringSources`...) — extrait le 29/08/2026 vers `apps/desktop/display_string_investigator.*` | ~1673 (L2707–4380) | **Bas** — quasi `const`, état minimal | — |
| [x] C2 | CLR/.NET inspector bridge (RPC vers process externe) — extrait le 29/08/2026 vers `apps/desktop/clr_inspector_bridge.*` | ~1108 (L14343–15451) | **Bas** — sous-système déjà séparé | — |
| [x] C3 | Driver kernel (`probe/startKernelDriver`, read/write kernel) — extrait le 29/08/2026 vers `apps/desktop/kernel_driver_manager.*` | ~288 (L15451–15739) | **Bas** | — |
| [x] C4 | UWP / save-file investigation — extrait le 29/08/2026 vers `apps/desktop/save_file_investigator.*` | ~291 (L2069–2360) | **Bas** | — |
| [x] C5 | Freeze-value / hotkeys globaux / overlay Trainer — extrait le 29/08/2026 vers `apps/desktop/freeze_hotkey_overlay_manager.*` | ~407 (L9738–10145) | **Bas** — objets de session déjà `unique_ptr` | — |
| [x] C6 | Code patching (AOB/patch/restore/hooks/AutoAssembler) — extrait le 29/08/2026 vers `apps/desktop/code_patch_manager.*` | ~1133 (L8605–9738) | **Bas** | C7 (debug cancellation), C10 (persistance profil) |
| [x] C7 | Breakpoints matériels / find-what-writes / page-guard / speedhack / API-hook — extrait le 29/08/2026 vers `apps/desktop/debug_feature_manager.*` | ~1494 (L6946–8440) | **Bas-moyen** | C6 |
| [x] C8 | Scanning core (exact/unknown/AOB/group) — extrait le 30/08/2026 vers `apps/desktop/scanning_core_manager.*` | ~2032 à l'origine | **Bas-moyen** — consomme `ScanStateAccess` (PHASE 223) au lieu du trio brut | C9 (clos), C10, C11 |
| [x] C9 | Write/freeze/rollback core — extrait le 30/08/2026 vers `apps/desktop/write_freeze_core_manager.*` | ~534 à l'origine, cluster relocalisé après PHASE 221/223 | **Bas-moyen** — les chemins write/rollback/watch consomment `AutoWriteStateAccess`; les écritures auto/chat/profils restent dans C12/C13/C10 | C10, C12, C13 |
| [x] C10 | Profils / pointer chains / Ghidra bridge / persistance Lua — extrait le 30/08/2026 (Codex) vers `apps/desktop/profile_manager.h/.cpp`, revue/smoke test/commit par Claude | ~2005 (L16325–18330) | **Moyen** — lit l'état de C6 et C9 pour persister | C6, C9 |
| [x] C11 | Automation pipe / settings / diagnostics — clos en C11a + C11b | dispersé (L13992–16325, L18330–18614) | **Bas-moyen** — le logger de télémétrie reste exposé via wrappers `ApplicationController` pour les appels transverses C12/C13 | C12, C13 |
| [x] C11a | └ Cycle de vie du pipe d'automatisation (`enable/disableAutomationMode`, `getAutomationPipeStatus`, `ensureAutomationPipeStartedIfConfigured`) — extrait le 29/08/2026 vers `apps/desktop/automation_pipe_manager.*` | ~66 | **Bas — réellement isolé** | — |
| [x] C11b | └ Reste de C11 : settings + diagnostics — extrait le 30/08/2026 vers `apps/desktop/settings_diagnostics_manager.h/.cpp` (`getSettings`/`saveSettings`/`getAiModelStatus`/`browseForModelFile`, chemins logs/debug/telemetry, smart-search events, log tail, export diagnostics, stockage temporaire) | dispersé | **Moyen, clos** — helpers settings dupliqués localement pour éviter le recouplage à C8 ; `appendSmartSearchDebug`/`appendScanTelemetry` restent appelables sur `ApplicationController` et délèguent au manager | C8 clos |
| C12 | Chat-memory write/freeze glue (`activateChatMemoryTargetsFromQuery`...) | ~478 (L10354–10832) | **Haut** | C13, C14, C8, C9 |
| C13 | `startAutoResolve` + escalade d'échec | ~475 (L11890–12365) | **Haut** | C12, C14 |
| C14 | `startSmartSearch` (dispatch chat/IA) | **~1627 lignes, une seule fonction** (L12365–13992) | **Haut — le pire du fichier** | C12, C13, C8, C10 |

**Ordre recommandé :** C1-C11 sont clos (C11 scindé en C11a/C11b, PHASE 219/232). Il reste C12/C13/C14 en dernier, comme un seul chantier groupé ("dispatch IA/chat") vu leur imbrication mutuelle — ne pas les répartir entre agents différents, ils se marchent dessus.

**Clôture première vague backend bas-couplage (29/08/2026)** : toute la première vague côté `ApplicationController` est close (`C1`, `C2`, `C3`, `C4`, `C5`, `C6`, `C7`). Les prochains candidats backend (`C8+`) demandent une interface d'accès partagée ou un couplage plus fort, donc ne sont plus des petits chantiers indépendants.

## Candidats d'extraction — `ui/src/stores/app.ts`

| # | Candidat | Lignes approx. | Couplage | Entangled avec |
| --- | --- | --- | --- | --- |
| [x] S1 | CLR Inspector (`clr*` refs + fonctions) — extrait le 29/08/2026 vers `ui/src/stores/clrInspector.ts` (en réalité couplage moyen : 7 fonctions dépendent de `confirmRiskAction`, non extrait — gate gardé dans `app.ts`, résultat délégué au store) | ~600 | **Bas** | — |
| [x] S2 | Speedhack / API hooking / blocage réseau — extrait le 29/08/2026 vers `ui/src/stores/speedhack.ts` | ~200 | **Bas** | — |
| [x] S3 | Automation pipe status — extrait le 29/08/2026 vers `ui/src/stores/automationPipe.ts` | ~60 | **Bas** | — |
| [x] S4 | Driver kernel (`kernelDriverStatus`, read/write) — extrait le 29/08/2026 vers `ui/src/stores/kernelDriver.ts` (`memoryAccessMode`/dispatch usermode-vs-kernel restés dans `app.ts`, pas spécifiques à ce candidat) | ~130 | **Bas** | — |
| [x] S5 | Action log (`addActionLog`) — extrait le 29/08/2026 vers `ui/src/stores/actionLog.ts` | ~75 + 282 sites d'appel | **Fondation partagée — à faire tôt, seul** | quasi tous |
| [x] S6 | Investigation timeline (`activeInvestigation`, `addInvestigationStep`) — extrait le 29/08/2026 vers `ui/src/stores/investigation.ts` | ~300 + 35 sites d'appel | **Fondation partagée — à faire tôt, seul** | quasi tous |
| [x] RiskGate | `confirmRiskAction`/`resolveRiskDialog`/`riskDialog`/`mutedRiskConfirmations`/`automationPipeDispatchDepth` — extrait le 29/08/2026 vers `ui/src/stores/riskGate.ts` (dernière fondation partagée) | ~140 + 39 sites d'appel | **Fondation partagée — à faire tôt, seul** | quasi tous |
| [x] S7 | Write/Freeze/Checkpoint (`executeCheckpoint*`) — extrait le 30/08/2026 vers `ui/src/stores/writeFreeze.ts` | ~1300 | **Haut** — vérifié en pratique : quasi chaque fonction retombe sur Session/Trainer (S8), Watch, Chat/SmartSearch (S10) ou le mode kernel, tous injectés une seule fois via `configureWriteFreezeContext` plutôt que déplacés/dupliqués | S8, S9 |
| [x] S8 | Trainer features — extrait le 30/08/2026 vers `ui/src/stores/trainer.ts` | ~900 | **Moyen-haut** — vérifié en pratique bien plus léger que redouté une fois S1/S9a déjà en feuilles indépendantes : import direct de `clrInspector.ts`/`scanning.ts`/`writeFreeze.ts`/`workspaceItems.ts`, seules 4 dépendances (`processName`/`confirmRiskAction`/`kernelMemoryModeActive`/`writeMemoryValueByMode`) injectées via `configureTrainerContext` | S1 (CLR), S9 |
| S9 | Profils / Workspace (bookmarks, templates, import/export) | ~1500 | **Haut** — bidirectionnel avec S7/S8 | S7, S8 |
| [x] S9a | └ CRUD pur templates de structure + bookmarks workspace (save/load/add/update/delete/clear) — extrait le 29/08/2026 vers `ui/src/stores/workspaceItems.ts` | ~250 | **Bas — réellement isolé** (seul `processName` traverse, en paramètre explicite) | — |
| S9b | └ Reste de S9 : profils, pointer chains, projets workspace, import/export JSON complet | ~1250 | **Haut** — bidirectionnel avec S7/S8, pas encore débloqué | S7, S8 |
| S10 | Chat / Smart Search | ~700 | **Haut** | S6, S7 |
| [x] S11a | └ Mécanique de scan pure (exact/next/unknown/groupe/chiffré) + pagination/sélection candidats pour affichage — extrait le 30/08/2026 vers `ui/src/stores/scanning.ts` | ~700 | **Bas — réellement isolé** (une seule dépendance transversale, `addAddressToWatch`, injectée une fois via callback plutôt que threadée par appel) | — |
| S11b | └ Reste de S11 : promotion candidat → cible d'écriture (`finalCandidateTargets`/`ignoredCandidateAddresses`/`keptCandidateAddresses`/`freezeCandidateCurrent`/`keepCandidate`/`ignoreCandidate`) et tout ce qui est piloté par le chat/IA (`runAutoEncryptedScan`, `runAutoUnknownObservation`, `runAutoTraceUiString`) | ~500 | **Haut** — S7 (write/freeze) et S10/C14 (dispatch chat/IA, jamais scindé) | S7, S10 |
| [x] S12 | Settings — extrait le 29/08/2026 vers `ui/src/stores/settings.ts` (`storeToRefs` pour garder les ~20 refs sous le même nom dans `app.ts`, donc les sites de lecture externes n'ont pas eu besoin de changer ; les 2 effets de bord vers d'autres domaines — sync `exactScanType`/`unknownScanType`, appel `refreshDiagnostics` — injectés en callbacks optionnels) | ~110 refs dispersés | **Bas en interne, mais lu par tous les autres domaines** — extraction = re-câblage de nombreux sites de lecture, pas un problème de logique | — |

**Ordre recommandé :** S1-S4 en premier (indépendants). S5 et S6 ensuite, **chacun par un seul agent** (ce sont des dépendances partagées, pas des domaines isolés — un split en cours de route par deux agents différents créerait des conflits de merge quasi garantis). S7 est clos depuis PHASE 229, S8 depuis PHASE 230 (tous deux extraits avec dépendances injectées, sans toucher S9/S10). S9/S10 restent, une fois S5/S6 stabilisés en modules séparés.

## Règle d'usage

- Ne pas ouvrir ce chantier "à froid" : l'extraction d'un candidat se fait **quand une phase produit touche déjà cette zone** pour une autre raison, dans la continuité du patron déjà utilisé côté Vue (`useExpertAobFlow.ts`, `useExpertPointerChain.ts`, `useExpertWriteSelection.ts`, `trainerDependencies.ts`).
- Chaque extraction doit rester vérifiable de la même façon que le reste du projet : `killengine_unit_tests.exe` si la logique est décrite comme testable en isolation, sinon vérification live (pipe/CDP) — `ApplicationController` n'a aujourd'hui aucune couverture unitaire directe, donc les candidats côté C++ (C1-C14) nécessitent une passe de vérification live après extraction, pas juste une compilation propre.
- Mettre à jour ce document (case cochée + date + fichier réel créé) à chaque extraction réalisée, comme `docs/POWER_UP_ROADMAP.md` le fait pour les capacités produit.
- Ne jamais toucher C12/C13/C14 (ou S9/S10 côté TS) en parallèle par deux agents différents — trop entremêlés, un seul agent à la fois sur ce sous-ensemble.
