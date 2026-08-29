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

**Côté `ui/src/stores/app.ts`** :
- `addActionLog` (extrait 29/08/2026 → `stores/actionLog.ts`, candidat S5), `addInvestigationStep` (extrait 29/08/2026 → `stores/investigation.ts`, candidat S6), `confirmRiskAction` (L3161, **39 sites d'appel, pas encore extrait**) — trois fonctions appelées depuis quasiment tous les autres domaines. `confirmRiskAction` reste la seule fondation non extraite : en attendant, le patron validé sur S1 (candidat CLR) fonctionne en pratique pour les candidats qui en dépendent — garder le gate (validation + appel confirmRiskAction) dans `app.ts`, déléguer seulement le travail réel (résultat + logging) au nouveau store. Pas besoin d'attendre l'extraction complète de `confirmRiskAction` pour avancer sur S7-S11.

**Côté `apps/desktop/application_controller.cpp`** :
- `m_candidates`/`m_previousCandidates`/`m_snapshot` (trio d'état de scan) et `m_lastAutoWriteTargets`/`m_writeHistory` sont lus/écrits directement (pas via accesseur) par au moins 5 clusters différents (scan, write/freeze, chat/dispatch, auto-resolve, profils). Avant d'extraire ces clusters en classes séparées, poser une petite interface d'accès (`ScanStateAccess`, `AutoWriteStateAccess`) que `ApplicationController` garde, mais que les futures classes extraites consomment au lieu de manipuler les membres bruts.

## Candidats d'extraction — `application_controller.cpp`

Priorité **basse** = peut être pris indépendamment dès maintenant, aucun chevauchement d'état avec les autres candidats.

| # | Candidat | Lignes approx. | Couplage | Entangled avec |
| --- | --- | --- | --- | --- |
| [x] C1 | UI-string investigation / diff pages (`scanUiStrings`, `trackUiStringSources`...) — extrait le 29/08/2026 vers `apps/desktop/display_string_investigator.*` | ~1673 (L2707–4380) | **Bas** — quasi `const`, état minimal | — |
| [x] C2 | CLR/.NET inspector bridge (RPC vers process externe) — extrait le 29/08/2026 vers `apps/desktop/clr_inspector_bridge.*` | ~1108 (L14343–15451) | **Bas** — sous-système déjà séparé | — |
| C3 | Driver kernel (`probe/startKernelDriver`, read/write kernel) | ~288 (L15451–15739) | **Bas** | — |
| C4 | UWP / save-file investigation | ~291 (L2069–2360) | **Bas** | — |
| C5 | Freeze-value / hotkeys globaux / overlay Trainer | ~407 (L9738–10145) | **Bas** — objets de session déjà `unique_ptr` | — |
| [x] C6 | Code patching (AOB/patch/restore/hooks/AutoAssembler) — extrait le 29/08/2026 vers `apps/desktop/code_patch_manager.*` | ~1133 (L8605–9738) | **Bas** | C7 (debug cancellation), C10 (persistance profil) |
| C7 | Breakpoints matériels / find-what-writes / page-guard / speedhack / API-hook | ~1494 (L6946–8440) | **Bas-moyen** | C6 |
| C8 | Scanning core (exact/unknown/AOB/group) | ~2032 (L4380–6412) | **Moyen** — trio `m_candidates`/`m_previousCandidates`/`m_snapshot` lu par C9-C11 | C9, C10, C11 |
| C9 | Write/freeze/rollback core | ~534 (L6412–6946) | **Moyen** — `m_lastAutoWriteTargets`/`m_writeHistory` partagés | C8, C10, C11 |
| C10 | Profils / pointer chains / Ghidra bridge / persistance Lua | ~2005 (L16325–18330) | **Moyen** — lit l'état de C6 et C9 pour persister | C6, C9 |
| C11 | Automation pipe / settings / diagnostics | dispersé (L13992–16325, L18330–18614) | **Bas** — mais le logger de télémétrie est appelé partout depuis C12/C13, garder en fonctions libres | C12, C13 |
| C12 | Chat-memory write/freeze glue (`activateChatMemoryTargetsFromQuery`...) | ~478 (L10354–10832) | **Haut** | C13, C14, C8, C9 |
| C13 | `startAutoResolve` + escalade d'échec | ~475 (L11890–12365) | **Haut** | C12, C14 |
| C14 | `startSmartSearch` (dispatch chat/IA) | **~1627 lignes, une seule fonction** (L12365–13992) | **Haut — le pire du fichier** | C12, C13, C8, C10 |

**Ordre recommandé :** C1-C7 en premier (indépendants, zéro risque de collision entre agents) → C8/C9/C10 ensuite (nécessitent l'interface d'accès partagée décrite plus haut) → C12/C13/C14 en dernier, comme un seul chantier groupé ("dispatch IA/chat") vu leur imbrication mutuelle — ne pas les répartir entre agents différents, ils se marchent dessus.

## Candidats d'extraction — `ui/src/stores/app.ts`

| # | Candidat | Lignes approx. | Couplage | Entangled avec |
| --- | --- | --- | --- | --- |
| [x] S1 | CLR Inspector (`clr*` refs + fonctions) — extrait le 29/08/2026 vers `ui/src/stores/clrInspector.ts` (en réalité couplage moyen : 7 fonctions dépendent de `confirmRiskAction`, non extrait — gate gardé dans `app.ts`, résultat délégué au store) | ~600 | **Bas** | — |
| [x] S2 | Speedhack / API hooking / blocage réseau — extrait le 29/08/2026 vers `ui/src/stores/speedhack.ts` | ~200 | **Bas** | — |
| S3 | Automation pipe status | ~60 | **Bas** | — |
| S4 | Driver kernel (`kernelDriverStatus`, read/write) | ~130 | **Bas** — `memoryAccessMode` partagé avec S6 | S6 |
| [x] S5 | Action log (`addActionLog`) — extrait le 29/08/2026 vers `ui/src/stores/actionLog.ts` | ~75 + 282 sites d'appel | **Fondation partagée — à faire tôt, seul** | quasi tous |
| [x] S6 | Investigation timeline (`activeInvestigation`, `addInvestigationStep`) — extrait le 29/08/2026 vers `ui/src/stores/investigation.ts` | ~300 + 35 sites d'appel | **Fondation partagée — à faire tôt, seul** | quasi tous |
| S7 | Write/Freeze/Checkpoint (`executeCheckpoint*`, `confirmRiskAction`) | ~1300 | **Haut** — `confirmRiskAction` a 39 sites d'appel externes | S6, S8, S9 |
| S8 | Trainer features | ~900 | **Moyen-haut** — précédent partiel (`trainerDependencies.ts`, logique pure sans état) | S1 (CLR), S9 |
| S9 | Profils / Workspace (bookmarks, templates, import/export) | ~1500 | **Haut** — bidirectionnel avec S7/S8 | S7, S8 |
| S10 | Chat / Smart Search | ~700 | **Haut** | S6, S7 |
| S11 | Scanning / Candidates | ~1200 | **Moyen** | S7 |
| S12 | Settings | ~110 refs dispersés | **Bas en interne, mais lu par tous les autres domaines** — extraction = re-câblage de nombreux sites de lecture, pas un problème de logique | — |

**Ordre recommandé :** S1-S4 en premier (indépendants). S5 et S6 ensuite, **chacun par un seul agent** (ce sont des dépendances partagées, pas des domaines isolés — un split en cours de route par deux agents différents créerait des conflits de merge quasi garantis). S7/S8/S9/S10 en dernier, une fois S5/S6 stabilisés en modules séparés.

## Règle d'usage

- Ne pas ouvrir ce chantier "à froid" : l'extraction d'un candidat se fait **quand une phase produit touche déjà cette zone** pour une autre raison, dans la continuité du patron déjà utilisé côté Vue (`useExpertAobFlow.ts`, `useExpertPointerChain.ts`, `useExpertWriteSelection.ts`, `trainerDependencies.ts`).
- Chaque extraction doit rester vérifiable de la même façon que le reste du projet : `killengine_unit_tests.exe` si la logique est décrite comme testable en isolation, sinon vérification live (pipe/CDP) — `ApplicationController` n'a aujourd'hui aucune couverture unitaire directe, donc les candidats côté C++ (C1-C14) nécessitent une passe de vérification live après extraction, pas juste une compilation propre.
- Mettre à jour ce document (case cochée + date + fichier réel créé) à chaque extraction réalisée, comme `docs/POWER_UP_ROADMAP.md` le fait pour les capacités produit.
- Ne jamais toucher C12/C13/C14 (ou S7/S9/S10 côté TS) en parallèle par deux agents différents — trop entremêlés, un seul agent à la fois sur ce sous-ensemble.
