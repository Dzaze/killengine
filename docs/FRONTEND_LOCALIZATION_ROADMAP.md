> **ATTENTION - Priorité Des Ordres Projet**
> Un ordre prioritaire explicite du propriétaire du projet prime sur les consignes temporaires de session des agents IA.

# KillEngine — Feuille de route de localisation de l'interface (Vue)

> Chantier **staffé** (09/09/2026). Voir `docs/PHASE_TRACKER.md` pour le pont vers cette feuille de route et la règle d'usage des roadmaps. Distinct de `docs/AI_CHAT_LOCALIZATION_ROADMAP.md` (clos) qui couvrait le texte **généré par le backend** dans le chat — ici il s'agit du texte **statique des templates Vue** (menus, boutons, libellés, placeholders, bannières d'aide).

## Origine (09/09/2026)

Après la clôture du chantier de localisation du chat IA, question posée par le propriétaire : "donc un anglais peut parler avec le chat sans problème ?". Réponse : oui côté chat (backend), mais un audit complémentaire du frontend a révélé un problème **bien plus large et jusque-là insoupçonné** — la bascule de langue dans Paramètres (`ui/language`) ne traduit quasiment aucune page.

**Constat de l'audit (agent Explore, lecture seule, 39 fichiers `.vue` sous `ui/src/views/` et `ui/src/components/`)** :
- `ui/src/i18n/locales/fr.json` et `en.json` existent, sont **parfaitement synchronisés** (624 clés chacun, aucune manquante des deux côtés) et sont donc fiables pour ce qu'ils couvrent déjà.
- Mais `$t(...)` n'est utilisé que **210 fois au total dans les templates**, concentré presque entièrement dans 4 fichiers (`NetworkView.vue` 51, `ModulesView.vue` 41, `WebView2InspectorView.vue` 19, `MemoryView.vue` 18).
- **21 des 39 fichiers Vue n'utilisent `$t()` nulle part dans leur template.**
- **37 des 39 fichiers** contiennent du texte français codé en dur (littéral, `title=`, `placeholder=`, `aria-label=`, ternaires, template strings) — **603 lignes** trouvées (sous-compte : plusieurs chaînes par ligne dans les ternaires).

Concrètement, en anglais, restent quasi intégralement en français : `AssistantView.vue`, `ExpertView.vue` (le plus gros, ~150 lignes, + ses ~16 sous-panneaux dans `components/expert/`), `InvestigationView.vue`, `SettingsView.vue` (~100 lignes), `TrainerView.vue`, `ProfileView.vue`, `ScriptingView.vue`, `ClrInspectorView.vue`, `MemoryHeatmapView.vue`, `MemoryTimelineView.vue`, `PatternLearningView.vue`, `SpeedhackView.vue`, `LexiconView.vue`, et des pans entiers de `ModulesView.vue`/`NetworkView.vue`/`WebView2InspectorView.vue` malgré leurs `$t()` déjà présents ailleurs dans le même fichier.

## Troisième poche de texte non traduit — messages de chat générés côté TypeScript (découvert 09/09/2026 en démarrant U1)

En commençant U1 (`AssistantView.vue`), découverte que le fichier fait en réalité **2115 lignes**, pas ~25 chaînes : son `<script setup>` contient **68 appels `store.pushMessage('assistant', "texte français")`** — des messages de chat construits côté frontend TypeScript (gestion des `recoveryActions`, retours d'erreur, confirmations), entièrement distincts du texte généré par le backend C++ (déjà couvert par `docs/AI_CHAT_LOCALIZATION_ROADMAP.md`, clos) et des libellés statiques de template (ce que l'audit initial de cette roadmap avait scanné, `<template>` uniquement).

**Le même pattern existe ailleurs** : `grep -rc "pushMessage(" ui/src/views ui/src/stores` remonte aussi 31 appels dans trois stores Pinia : `ui/src/stores/app.ts` (5), `ui/src/stores/assistantSmartSearch.ts` (20), `ui/src/stores/writeFreeze.ts` (6) — des fichiers `.ts` qui n'étaient même pas dans le périmètre de l'audit initial (`.vue` uniquement).

**Décision propriétaire (09/09/2026)** : élargir la roadmap plutôt que de la traiter comme un chantier séparé — chaque candidat de vue qui a un `<script setup>` avec du texte de chat en dur doit aussi traiter son script, pas seulement son template. Nouveaux candidats U20-U22 ajoutés ci-dessous pour les 3 stores.

## Deux systèmes d'aide distincts — ne pas les confondre

L'audit a remonté un piège : il existe **deux mécanismes d'aide séparés** dans l'UI, à ne pas mélanger pendant la traduction :

1. **`PanelIntro.vue`** (bannière "à quoi ça sert ?" en haut de chaque vue, props `what=`/`purpose=`/`how=`) — **entièrement hors i18n aujourd'hui**, chaîne françaises passées en dur depuis le composant parent à chaque usage. Aucune clé existante à réutiliser ici : il faut en créer.
2. **`InfoDot.vue` + le dictionnaire `help.*`** (icône "ⓘ" inline dans certains panneaux Expert, contenu riche `title`/`what`/`when`/`cost`/`example`) — **déjà entièrement câblé sur i18n et fonctionnel**, ne rien y toucher. Confirmé consommé par `AobSignaturePanel.vue`, `CandidatePanel.vue`, `ExactScanPanel.vue`, `InfoDot.vue`, `PointerChainScanPanel.vue`, `UnknownScanPanel.vue`, `WritePanel.vue`, `ExpertView.vue`.

## Décisions de méthode

1. **Mécanisme de traduction — `vue-i18n`, déjà en place** (`ui/src/i18n/index.ts`, `locales/{fr,en}.json`). Pas de nouveau système : chaque chaîne hardcodée devient une clé dans les deux fichiers de locale, consommée via `$t('namespace.key')` dans le template (ou `t('namespace.key')` côté `<script setup>` pour les chaînes utilisées dans des fonctions/computed).
2. **Convention de nommage des clés** : réutiliser le namespace racine existant quand il correspond déjà à la vue (`process`, `memory`, `network`, `modules`, `webview2`, `lexicon`, `findWhatAccesses`). Créer un nouveau namespace top-level par vue/composant sinon, en `camelCase` proche du nom de fichier (`assistant`, `expert`, `investigation`, `settings`, `trainer`, `profile`, `scripting`, `clrInspector`, `memoryHeatmap`, `memoryTimeline`, `patternLearning`, `speedhack`). Les sous-panneaux `components/expert/*Panel.vue` gardent chacun leur propre namespace top-level (cohérent avec `findWhatAccesses` déjà top-level, pas nesté sous `expert.*`), ex. `aobSignature`, `candidatePanel`, `exactScan`, `sessionPanel`, etc.
3. **Chaînes avec compte/pluriel** (ex. `"{{ n }} adresse(s) active(s)"`) : utiliser la pluralisation native de vue-i18n (`$tc`/le paramètre `count` selon la version installée) plutôt que le bricolage `(s)` actuel — à vérifier au moment de l'implémentation quelle forme l'app utilise déjà ailleurs, si utilisée.
4. **Ternaires** (`isRunning ? "Arrêter" : "Démarrer"`) : une clé par branche (`xxx.stop`/`xxx.start`), jamais une seule clé avec un `?:` sur le texte traduit.
5. **`PanelIntro` (`what=`/`purpose=`/`how=`)** : nouvelles clés à créer par vue, distinctes du dictionnaire `help.*` existant (voir section précédente) — proposer un sous-namespace dédié, ex. `assistant.intro.what/purpose/how`, plutôt que de les mélanger dans `help.*` qui a une forme différente (title/what/when/cost/example) et un usage différent (InfoDot, pas PanelIntro).
6. **Ne pas toucher** : les 210 `$t()` déjà en place, le dictionnaire `help.*` (déjà bon), et tout texte qui vient réellement du backend (`{{ someBackendField }}`) — ce texte est déjà couvert par `docs/AI_CHAT_LOCALIZATION_ROADMAP.md` s'il transite par le chat, ou hors périmètre sinon.
7. **Balayage en deux passes obligatoire**, même leçon que le chantier backend : un premier passage rate régulièrement des chaînes courtes ou des ternaires imbriqués. Toujours refaire un balayage de contrôle après la première traduction d'un fichier.
8. **Vérification** : `npm run type-check` + `npm run build` côté `ui/`, puis vérification visuelle réelle (CDP screenshot ou lancement de l'app + bascule Paramètres > Langue) — contrairement au chantier backend, il n'y a pas de suite de tests unitaires C++ à faire tourner ici, donc la vérification visuelle est la seule preuve fiable qu'une chaîne s'affiche bien traduite.
9. **`scripts/build.ps1` ne reconstruit jamais le frontend** — il empaquette `ui/dist` tel quel. Toujours relancer `npm run build` dans `ui/` avant une vérification visuelle, et **impérativement après chaque merge multi-agents sur ce chantier** (même sans conflit git) : le `ui/dist` du dossier principal reste sinon celui du dernier `npm run build` local, pas celui du code fusionné.

## Candidats (priorisés par fréquence d'usage réelle, pas par ordre de fichier)

| # | Candidat | Fichier(s) | Chaînes trouvées (audit) | `$t()` déjà présent ? | Priorité | Statut |
| --- | --- | --- | --- | --- | --- | --- |
| [x] U1 | Assistant (vue principale du chat) — template + script (`pushMessage`, `workflowLabel`, `safeStepLabel`, etc.) | `ui/src/views/AssistantView.vue` | 216 clés `assistant.*` ajoutées (template + les 68 `pushMessage` + `workflowLabel`/`safeStepLabel`/`confidenceFor`/etc. du script) | Non → complet | **Haute** — vue la plus utilisée | **Fait, testé (09/09/2026, Claude)** |
| [x] U2 | Processus (attache, mode d'accès mémoire) | `ui/src/views/ProcessView.vue` | 22 clés `process.*` ajoutées (namespace existant réutilisé), intro + filtre + mode d'accès + actions driver kernel | Non → complet | **Haute** — première vue vue par un nouvel utilisateur (démarrage) | **Fait, testé (09/09/2026, Codex)** |
| [x] U3 | Mode Expert — vue principale | `ui/src/views/ExpertView.vue` | 212 clés `expert.*` ajoutées (fichier de 4675 lignes, mais ~150-160 lignes de texte réel comme estimé — le reste est de la logique TS) | Non → complet | Haute | **Fait, testé (10/09/2026, Claude)** |
| [x] U3a | └ `AobSignaturePanel.vue` | `ui/src/components/expert/AobSignaturePanel.vue` | 52 clés `aobSignature.*` (+ 1 clé `bytesCount` ajoutée après-coup, 3 usages "o"/octets oubliés par Codex) | Non (aide InfoDot oui, template non) → complet | Moyenne | **Fait, testé (10/09/2026, Codex + gap fix Claude)** |
| [x] U3b | └ `AutoDissectPanel.vue` | `ui/src/components/expert/AutoDissectPanel.vue` | 16 clés `autoDissect.*` | Non → complet | Basse | **Fait, testé (10/09/2026, Codex)** |
| [x] U3c | └ `CandidatePanel.vue` | `ui/src/components/expert/CandidatePanel.vue` | 38 clés `candidatePanel.*` | Non (idem) → complet | Moyenne | **Fait, testé (10/09/2026, Codex)** |
| [x] U3d | └ `ExactScanPanel.vue` | `ui/src/components/expert/ExactScanPanel.vue` | 27 clés `exactScan.*` | Non (idem) → complet | Moyenne | **Fait, testé (10/09/2026, Codex)** |
| [x] U3e | └ `FindWhatAccessesPanel.vue` | `ui/src/components/expert/FindWhatAccessesPanel.vue` | 8 clés `findWhatAccesses.*` (namespace existant étendu) | Non → complet | Basse | **Fait, testé (10/09/2026, Codex)** |
| [x] U3f | └ `InjectionPanel.vue` | `ui/src/components/expert/InjectionPanel.vue` | 52 clés `injectionPanel.*` | Non → complet | Basse | **Fait, testé (10/09/2026, Codex)** |
| [x] U3g | └ `NextScanPanel.vue` | `ui/src/components/expert/NextScanPanel.vue` | 8 clés `nextScanPanel.*` | Non → complet | Basse | **Fait, testé (10/09/2026, Codex)** |
| [x] U3h | └ `PointerChainScanPanel.vue` | `ui/src/components/expert/PointerChainScanPanel.vue` | 25 clés `pointerChainScan.*` | Non (idem) → complet | Basse | **Fait, testé (10/09/2026, Codex)** |
| [x] U3i | └ `PointerChainWatchPanel.vue` | `ui/src/components/expert/PointerChainWatchPanel.vue` | 16 clés `pointerChainWatchPanel.*` | Non → complet | Basse | **Fait, testé (10/09/2026, Codex)** |
| [x] U3j | └ `RegionPanel.vue` | `ui/src/components/expert/RegionPanel.vue` | 5 clés `regionPanel.*` (4 ajoutées après-coup — Codex n'avait traité que le titre, 4 labels de métriques `Début`/`Fin`/`Protection`/`État` oubliés) | Non → complet | Basse | **Fait, testé (10/09/2026, Codex + gap fix Claude)** |
| [x] U3k | └ `SaveFilesPanel.vue` | `ui/src/components/expert/SaveFilesPanel.vue` | 21 clés `saveFilesPanel.*` (3 ajoutées après-coup : "Aucun fichier de sauvegarde probable trouvé.", bouton "Annuler", compteurs "fichier(s)"/"valeur(s)" oubliés par Codex) | Non → complet | Basse-moyenne | **Fait, testé (10/09/2026, Codex + gap fix Claude)** |
| [x] U3l | └ `SessionPanel.vue` | `ui/src/components/expert/SessionPanel.vue` | 24 clés `sessionPanel.*` | Non → complet | Basse-moyenne | **Fait, testé (10/09/2026, Codex)** |
| [x] U3m | └ `UnknownScanPanel.vue` | `ui/src/components/expert/UnknownScanPanel.vue` | 26 clés `unknown.*` (namespace existant étendu) | Non (idem) → complet | Basse-moyenne | **Fait, testé (10/09/2026, Codex)** |
| [x] U3n | └ `WatchLivePanel.vue` | `ui/src/components/expert/WatchLivePanel.vue` | 9 clés `watchLivePanel.*` | Non → complet | Basse | **Fait, testé (10/09/2026, Codex)** |
| [x] U3o | └ `WritePanel.vue` | `ui/src/components/expert/WritePanel.vue` | 38 clés `write.*` (namespace existant étendu) | Non (idem) → complet | Basse | **Fait, testé (10/09/2026, Codex)** |
| [x] U3p | └ `GroupScanPanel.vue` | `ui/src/components/expert/GroupScanPanel.vue` | 15 clés `groupScanPanel.*` | Non → complet | Basse | **Fait, testé (10/09/2026, Codex)** |
| [x] U3q | └ `ActionLogPanel.vue` | `ui/src/components/expert/ActionLogPanel.vue` | 2 clés `actionLogPanel.*` | Non → complet | Basse | **Fait, testé (10/09/2026, Codex)** |
| [x] U4 | Investigation (carnet d'hypothèses, timeline) | `ui/src/views/InvestigationView.vue` | 107 clés `investigation.*` ajoutées (template + script : `statusLabel`, `checkpointDetail`, `checkpointKindLabel`, `checkpointRiskLabel`, `statusText`, etc.) | Non → complet | Haute | **Fait, testé (10/09/2026, Claude)** |
| [x] U5 | Réglages | `ui/src/views/SettingsView.vue` | 278 clés `settings.*` ajoutées (fichier de 2092 lignes ; couvre 21 sections : interface, scan, stockage temporaire, IA locale, backend IA externe, workspace IA/Trainer, état, diagnostic, antivirus, driver kernel, Automation, Stealth, WebView2 CDP x2, log, événements Smart Search, session) | Non → complet | Haute | **Fait, testé (10/09/2026, Claude)** |
| [ ] U5a | └ `AssistantToolsPanel.vue` (sous-panneau Réglages) | `ui/src/components/settings/AssistantToolsPanel.vue` | ~10 | Non | Basse | Pas commencé |
| [x] U6 | Trainer | `ui/src/views/TrainerView.vue` | 76 clés `trainer.*` ajoutées | Non → complet | Moyenne-haute | **Fait, testé (10/09/2026, Codex)** |
| [x] U7 | Profils | `ui/src/views/ProfileView.vue` | 169 clés `profile.*` ajoutées (audit initial sous-estimé : 130 lignes FR réelles, pas ~10 — profils/cibles/patchs trainer/pont Ghidra/pointer maps) | Non → complet | Moyenne | **Fait, testé (10/09/2026, Claude)** |
| [x] U8 | Scripting (Lua) | `ui/src/views/ScriptingView.vue` | 42 clés `scripting.*` ajoutées | Non → complet | Moyenne | **Fait, testé (10/09/2026, Codex)** |
| [ ] U9 | CLR Inspector | `ui/src/views/ClrInspectorView.vue` | ~25 | Non | Moyenne | Pas commencé |
| [ ] U10 | Mémoire (hex viewer) — reste | `ui/src/views/MemoryView.vue` | ~13 restants (18 `$t()` déjà en place) | Partiel | Basse | Pas commencé |
| [ ] U11 | Memory Heatmap | `ui/src/views/MemoryHeatmapView.vue` | ~20 | Non | Basse-moyenne | Pas commencé |
| [ ] U12 | Memory Timeline | `ui/src/views/MemoryTimelineView.vue` | ~15 | Non | Basse-moyenne | Pas commencé |
| [ ] U13 | Pattern Learning | `ui/src/views/PatternLearningView.vue` | ~20 | Non | Basse | Pas commencé |
| [x] U14 | Speedhack | `ui/src/views/SpeedhackView.vue` | 20 clés `speedhack.*` ajoutées | Non → complet | Basse-moyenne | **Fait, testé (10/09/2026, Codex)** |
| [ ] U15 | Réseau — reste | `ui/src/views/NetworkView.vue` | ~15 restants (51 `$t()` déjà en place) | Partiel | Basse | Pas commencé |
| [ ] U16 | Modules — reste (guide EDR notamment) | `ui/src/views/ModulesView.vue` | ~12 restants (41 `$t()` déjà en place) | Partiel | Basse | Pas commencé |
| [ ] U17 | WebView2 Inspector — reste | `ui/src/views/WebView2InspectorView.vue` | ~3 (bannière intro) | Partiel (19 `$t()` déjà en place) | Basse | Pas commencé |
| [x] U18 | Lexique | `ui/src/views/LexiconView.vue` | 1 clé `lexicon.description` ajoutée (namespace `lexicon` déjà existant réutilisé) | Non → complet | Basse | **Fait, testé (10/09/2026, Codex)** |
| [x] U19 | `PanelIntro.vue` (partagé par toutes les vues) | `ui/src/components/common/PanelIntro.vue` | 4 clés `common.panelIntro.*` (les 3 labels + `aria-label`) | Non → complet | **Bloquant léger** — voir note | **Fait, testé (09/09/2026, Codex)** |
| [ ] U20 | Store `app.ts` — messages `pushMessage` | `ui/src/stores/app.ts` | ~5 | Non | Moyenne | Pas commencé |
| [ ] U21 | Store `assistantSmartSearch.ts` — messages `pushMessage` | `ui/src/stores/assistantSmartSearch.ts` | ~20 | Non | Moyenne-haute — même famille que U1 | Pas commencé |
| [ ] U22 | Store `writeFreeze.ts` — messages `pushMessage` | `ui/src/stores/writeFreeze.ts` | ~6 | Non | Moyenne | Pas commencé |

**Note U19** : `PanelIntro.vue` lui-même n'a que 2 chaînes fixes à traduire, mais ses props `what=`/`purpose=`/`how=` sont câblées en dur depuis **chaque vue appelante** (U1 à U18) — donc U19 se fait mécaniquement en même temps que chaque vue traitée, pas comme un chantier séparé. Le traiter en premier (le composant lui-même) débloque juste la convention de clé (`intro.what`/`intro.purpose`/`intro.how` par namespace de vue) que les autres candidats réutiliseront.

## Ordre recommandé

1. **U19 d'abord** (mécanique, 5 minutes) pour fixer la convention de clé `<namespace>.intro.*` utilisée par tous les candidats suivants.
2. **U1, U2** ensuite — vues à plus fort trafic (Assistant = usage quotidien, Process = premier écran vu).
3. **U3 (Expert + ses 16 sous-panneaux)** — le plus gros morceau, à répartir entre plusieurs agents en parallèle (fichiers disjoints, comme pour le chantier backend) une fois U1/U2 clos et la convention de clé validée en pratique sur au moins une vue complète.
4. **U4-U9** (Investigation, Settings, Trainer, Profils, Scripting, CLR Inspector) — vues secondaires mais complètes, à prendre dans n'importe quel ordre selon disponibilité agent.
5. **U10-U18** — résidus de fichiers déjà partiellement traduits + fichiers à faible trafic (Heatmap, Timeline, Pattern Learning, Speedhack, Lexique) : faible priorité, chantier permanent comme `docs/REFACTOR_ROADMAP.md`.

## Progrès (09/09/2026)

**U19, U1, U2 clos, premier round de travail parallèle sur ce chantier** : Codex sur U19 (`PanelIntro.vue`) + U2 (`ProcessView.vue`) dans un worktree séparé (`../killengine-codex-ui-u19-u2`), Claude sur U1 (`AssistantView.vue`) dans le dossier principal — fichiers disjoints, aucune collision.

- **U1** : 216 clés `assistant.*` ajoutées. Couvre le template ET le script `<script setup>` (les 68 appels `pushMessage`, `workflowLabel`, `safeStepLabel`, `confidenceFor`, `valueHistoryFor`, `writeSummaryFor`, `filteredCandidatesFor`, les tableaux de messages "thinking"). Vérifié `npm run type-check` + `npm run build` OK, puis **vérification visuelle réelle** via CDP (`Page.captureScreenshot` + `Runtime.evaluate` pour naviguer/basculer la langue, voir [[killengine_cdp_screenshot_technique]]) : capture FR et EN comparées côte à côte, tout le contenu dans le périmètre de ce fichier s'affiche bien traduit.
- **U2 + U19** : 22 clés `process.*` (namespace existant réutilisé) + 4 clés `common.panelIntro.*`. Vérifié par Claude après coup (pas juste le rapport de Codex) : `npm run type-check` + `npm run build` OK dans le worktree, balayage de contrôle sans trouver de chaîne française oubliée, valeurs FR/EN comparées une à une (aucune identique suspecte, les 4 valeurs identiques trouvées — "Modules", "Standard", "Kernel", "{count} modules" — sont des termes techniques partagés, pas des oublis).
- **Résidu visible en live, hors périmètre de U1/U2/U19** : les cartes d'exemple "Valeur directe"/"Valeur inconnue"/etc. dans l'écran d'accueil de l'Assistant restent en français — elles viennent de `store.workflowPresets`, défini dans `ui/src/stores/assistantSmartSearch.ts` (candidat U21, pas encore traité). Confirme que le découpage par candidat de cette roadmap correspond bien aux frontières réelles du code.
- **Piège de process trouvé pendant la vérification live, à refaire systématiquement** : `scripts/build.ps1` (le build C++ complet) **ne reconstruit jamais le frontend** — il utilise tel quel le contenu déjà présent dans `ui/dist`. Après un merge combinant le travail de deux agents (chacun ayant fait son propre `npm run build` dans son worktree respectif), le `ui/dist` du dossier principal restait celui du tout premier build (un seul des deux candidats visible). Un premier passage de vérification live a donc montré à tort `ProcessView.vue` toujours en français après merge. Diagnostiqué en quelques minutes (pas une vraie régression du merge), corrigé en relançant `npm run build` dans `ui/` sur l'état fusionné avant de relancer l'app — **toujours refaire `npm run build` après un merge multi-agents sur ce chantier, avant toute vérification visuelle**, même si `git merge` s'est fait sans conflit.

## Progrès (10/09/2026) — U3 clos entièrement (vue principale + 17 sous-panneaux)

Deuxième round de travail parallèle : Codex sur les 17 sous-panneaux `components/expert/*.vue` (U3a-U3q) dans un worktree séparé (`../killengine-codex-ui-u3-panels`), Claude sur `ExpertView.vue` (U3, 212 clés `expert.*`) dans le dossier principal — fichiers disjoints entre les deux lots de `.vue`, mais **les deux ont modifié `fr.json`/`en.json`** (chacun ajoutant ses propres namespaces), ce qui a produit un conflit git sur ces deux fichiers au merge — résolu par un merge JSON programmatique (union récursive des deux arborescences, aucune clé perdue, vérifié par script : 32 namespaces top-level, comptes de clés FR/EN identiques partout, 1442 clés terminales de chaque côté).

- **Codex** a traité les 17 fichiers en un commit (`8bc4c19`), avec sa propre vérification (type-check, build, `scripts/build.ps1`, suite de tests C++ 469/469, vérification visuelle CDP FR+EN attaché sur `KillEngineTestTarget.exe`).
- **Vérification indépendante par Claude** (avant ET après merge, cf. [[feedback_verify_dont_trust_agent_build_claims]]) a trouvé **7 chaînes manquées malgré la vérification visuelle de Codex** :
  - `RegionPanel.vue` : 4 labels de métriques (`Début`/`Fin`/`Protection`/`État`) — seul le titre du panneau avait été traduit.
  - `SaveFilesPanel.vue` : message vide "Aucun fichier de sauvegarde probable trouvé.", bouton "Annuler", et 2 compteurs `{{ n }} fichier(s)`/`{{ n }} valeur(s)`.
  - `AobSignaturePanel.vue` : 3 occurrences de l'unité `{{ formatNumber(x) }} o` (octets) non passées par i18n (résultats de scan AOB/patch).
  - **Hors périmètre `.vue` — trouvées seulement en vérifiant visuellement le bouton d'écriture en anglais** : `ui/src/composables/useExpertWriteSelection.ts` (label du bouton "Écrire"/"Écrire {n}" et résumé de sélection "{n} adresse(s) sélectionnée(s) · ...") et `ui/src/stores/app.ts` (texte de statut global sidebar "Prêt"/"Déconnecté"/"Attaché: {name}", visible sur **toutes** les vues, pas seulement Expert) — ces deux fichiers `.ts` ne sont pas des fichiers `.vue` de panneau, donc en dehors du périmètre strict de U3 tel que scanné par l'audit initial (`.vue` uniquement), mais le texte qu'ils produisent s'affiche bien dans l'UI Expert et ailleurs. Corrigés par un import direct de l'instance i18n globale (`import { i18n } from '@/i18n'`, puis `i18n.global.t(...)`) plutôt que `useI18n()` (qui exige un contexte de composant Vue, indisponible dans un composable/store appelé hors `setup()`).
- **Leçon de méthode** : la vérification visuelle CDP doit couvrir explicitement le changement de langue sur des éléments transverses (statut sidebar, labels de boutons dynamiques calculés dans des composables/stores), pas seulement le contenu statique des templates des fichiers modifiés — un agent qui vérifie "son" fichier ne verra pas un bug qui vit dans un fichier partagé en amont.
- **Nouveau pattern de code établi** : pour traduire du texte généré dans un fichier `.ts` hors composant (composable top-level, store Pinia), utiliser `import { i18n } from '@/i18n'` puis `i18n.global.t(...)` — `useI18n()` de vue-i18n ne fonctionne que dans un contexte `setup()`. Ce pattern sera réutilisable pour U20-U22 (stores `app.ts`/`assistantSmartSearch.ts`/`writeFreeze.ts`).
- Validations complètes après merge + gap fixes : `npm run type-check` OK, `npm run build` OK, suite de tests C++ **469/469 OK** (aucun changement C++ ce round, vérification de non-régression), vérification visuelle CDP FR et EN en direct avec attache réelle sur `KillEngineTestTarget.exe` (nav, scan, write/freeze, AOB, injection, pointeurs, journal actions).

## Progrès (10/09/2026) — U4 clos (InvestigationView.vue)

Troisième round, solo cette fois (pas de travail parallèle avec Codex sur ce round). 107 clés `investigation.*` ajoutées, couvrant le template ET le `<script setup>` (fonctions `statusLabel`, `checkpointTitle`, `checkpointDetail`, `checkpointKindLabel`, `checkpointScoreLabel`, `checkpointRiskLabel`, `statusText`, `bestNextAction`, `notebookSections` — toutes retournaient des chaînes françaises en dur consommées ensuite par le template).

- Vérifié avant de commencer que le fichier ne suivait pas le pattern piège d'U1 (`AssistantView.vue`) : aucun appel `pushMessage`, ~45 lignes de texte français réel (accents), conforme à l'estimation de l'audit initial (~40) — pas de surprise de périmètre cette fois.
- **Vérification visuelle réelle particulièrement complète** : contrairement aux vérifications précédentes de ce chantier (qui tombaient souvent sur l'état vide "aucune enquête"), une vraie session d'enquête active était présente au moment du test (137 étapes, objectif `777777`, process `KillEngineTestTarget.exe`) — a permis de vérifier en conditions réelles la timeline, les filtres, le rapport IA, "Meilleure prochaine action"/"Best next action", et les sections UWP/Archives, en FR puis en EN.
- **Distinction importante confirmée en vérifiant** : le contenu généré par le backend C++ (résumé du rapport IA "0 candidat(s), 0 cible(s) active(s)...", titres/détails des étapes de la timeline "Risque confirmé: Activer...") reste en français dans les deux langues de l'UI — attendu et hors périmètre de ce chantier (règle n°6 de la méthode, ce texte relève de `docs/AI_CHAT_LOCALIZATION_ROADMAP.md` ou est simplement hors périmètre), pas un oubli.
- Validations : `npm run type-check` OK, `npm run build` OK, 111 références de clés uniques toutes résolues des deux côtés par script, suite C++ 469/469 (sanity check, aucun changement C++), vérification visuelle CDP FR et EN complète (en-tête, 2 bannières PanelIntro, carnet d'hypothèses, résumé, rapport IA, meilleure action, timeline + filtres, hypothèses/checkpoints, état UWP, archives).

## Progrès (10/09/2026) — U5 clos (SettingsView.vue), quatrième round en parallèle avec Codex sur U8/U14/U18

Quatrième round : Claude sur `SettingsView.vue` (U5, le plus gros fichier de vue traité jusqu'ici hors Expert) dans le dossier principal, Codex sur U8 (`ScriptingView.vue`)/U14 (`SpeedhackView.vue`)/U18 (`LexiconView.vue`) dans un worktree séparé (`../killengine-codex-ui-u8-u14-u18`) — fichiers disjoints.

- **U5** : 278 clés `settings.*` ajoutées, fichier de 2092 lignes couvrant 21 sections (Interface, Scan, Stockage temporaire, IA locale, Backend IA externe, Workspace IA/Trainer, État, Diagnostic, Compatibilité antivirus, Driver kernel avec son parcours pédagogique en 4 étapes, Mode Automation, Mode Stealth, Débogage CDP WebView2, Préparer l'inspection WebView2, Log principal, Événements Smart Search, Session). Script ET template traduits : `runtimeRows`, `kernelCapabilityLabel`, `kernelLearningSteps`, `formatBytes` (unités Go/Mo/Ko/o), `eventSummary`, messages de statut export/import.
- **Piège de méthode rencontré et corrigé en cours de route** : deux phrases avaient un mot mis en gras au milieu (`<strong>Mode Automation</strong>`, `<strong>tous les hôtes WebView2...</strong>`) — la première tentative a interpolé une clé `$t()` avec un paramètre vide puis `.split()` sur le résultat pour extraire préfixe/suffixe côté template, un bricolage fragile. Corrigé en clés préfixe/suffixe dédiées (`xxxPrefix`/`xxxSuffix`) dès que remarqué — pattern à réutiliser directement la prochaine fois qu'une traduction a besoin d'un mot en gras au milieu d'une phrase, plutôt que de repasser par le bricolage `.split()`.
- Validations : `npm run type-check` OK, `npm run build` OK, 278 références de clés uniques toutes résolues des deux côtés par script, suite C++ 469/469 (sanity check), vérification visuelle CDP FR et EN complète (5 captures par langue, du haut de page jusqu'au bas — Interface/Scan/Stockage jusqu'à Session en passant par IA locale, Workspace, Driver kernel, Stealth, WebView2, Log).

## Progrès (10/09/2026) — U8, U14, U18 clos (Codex, en parallèle du round U5)

Résultat du lot confié à Codex pendant que Claude traitait U5 (`SettingsView.vue`) — voir l'entrée ci-dessus. 3 petits fichiers disjoints traités en un commit (`2363a20`) :

- **U8** (`ScriptingView.vue`) : 42 clés `scripting.*` — template + script (`statusLabel` computed).
- **U14** (`SpeedhackView.vue`) : 20 clés `speedhack.*` — template + script (`statusLabel` computed).
- **U18** (`LexiconView.vue`) : 1 clé `lexicon.description` ajoutée, namespace `lexicon` déjà existant réutilisé (seule chaîne restante hors du système i18n).

**Vérification indépendante par Claude avant merge** (cf. [[feedback_verify_dont_trust_agent_build_claims]]) : diff complet des 3 fichiers `.vue` relu ligne par ligne, comptes de clés par namespace vérifiés par script (fr/en identiques : scripting 42/42, speedhack 20/20, lexicon.description présent des deux côtés), 66 références de clés uniques toutes résolues des deux côtés, `help.*` confirmé byte-identique (non touché), `npm run type-check` + `npm run build` OK dans le worktree de Codex avant merge. **Aucun gap trouvé cette fois** — contrairement aux deux rounds précédents avec Codex (U2/U19 et U3), ce lot de fichiers plus petits et plus simples n'a laissé aucune chaîne oubliée.

**Merge** : conflit git attendu sur `fr.json`/`en.json` (Codex avait ajouté `scripting.*`/`speedhack.*`/`lexicon.description` sur la même branche de départ que le `settings.*` de Claude) — résolu par le même merge JSON programmatique que les rounds précédents (union récursive, zéro perte, vérifié : 1904 clés terminales de chaque côté après fusion).

**Comment vérifié après merge** : `npm run type-check` + `npm run build` OK sur l'état fusionné, suite C++ **469/469 OK**, vérification visuelle CDP FR et EN complète pour les 3 vues (Lua, Speedhack, Lexique) sur l'état fusionné. Worktree et branche temporaires supprimés après merge.

## Progrès (10/09/2026) — U6 et U7 clos (cinquième round, Codex + Claude)

Cinquième round : Codex sur U6 (`TrainerView.vue`) dans un worktree séparé (`../killengine-codex-ui-u6`), Claude sur U7 (`ProfileView.vue`) dans le dossier principal — fichiers disjoints.

- **U7** (Claude) : audit initial sous-estimait fortement le périmètre (~10 chaînes estimées contre 130 lignes de texte français réel) — fichier de 1776 lignes avec ~90 messages de statut construits dynamiquement côté script (`String.raw`/template literals avec préfixes ✓/✗/⚠). 169 clés `profile.*` ajoutées ; les icônes de préfixe restent des littéraux JS, seul le corps du message est traduit (ex. `'⚠ ' + t('profile.selectOrCreateProfileFirst')`).
- **U6** (Codex) : 76 clés `trainer.*` ajoutées. Vérification indépendante par Claude avant merge (cf. [[feedback_verify_dont_trust_agent_build_claims]]) : diff relu intégralement, comptes de clés fr/en identiques (76/76), 76 références de clés toutes résolues, `help.*` byte-identique (non touché), build indépendant OK dans le worktree — **aucun gap trouvé**, deuxième round consécutif sans oubli après U8/U14/U18.
- **Résidu confirmé hors périmètre lors de la vérification live** : les boutons de scénarios ("Argent / Or", "Vie / PV", "Score / Niveau", "Munitions") restent en français même en anglais dans TrainerView.vue — proviennent de `store.workflowPresets` (candidat U21, pas encore traité), pas un oubli de Codex. Même résidu déjà documenté lors du round U1.
- **Merge** : conflit git attendu sur `fr.json`/`en.json` (même branche de départ que le `profile.*` de Claude) — résolu par le même merge JSON programmatique (union récursive, zéro perte, 2155 clés terminales de chaque côté après fusion).
- **Comment vérifié** : `npm run type-check` + `npm run build` OK sur l'état fusionné, suite C++ **469/469 OK**, vérification visuelle CDP FR et EN complète pour les deux vues (ProfileView avec 2 profils réels dont un avec patch actif, TrainerView avec une feature réelle). Worktree et branche temporaires supprimés après merge.

## Règle d'usage

Même règle que `docs/AI_CHAT_LOCALIZATION_ROADMAP.md` et `docs/REFACTOR_ROADMAP.md` : mettre à jour ce document (case cochée + date + nombre réel de chaînes traitées) à chaque candidat clos, journaliser dans `docs/PHASE_TRACKER.md`. Contrairement au chantier backend, la vérification se fait par `npm run type-check`/`npm run build` + contrôle visuel réel (pas de suite de tests unitaires côté frontend pour ces vues), donc un candidat n'est "fait" qu'après une vérification visuelle en direct (CDP screenshot ou lancement app), pas juste après une compilation propre.
