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

## Candidats (priorisés par fréquence d'usage réelle, pas par ordre de fichier)

| # | Candidat | Fichier(s) | Chaînes trouvées (audit) | `$t()` déjà présent ? | Priorité | Statut |
| --- | --- | --- | --- | --- | --- | --- |
| [x] U1 | Assistant (vue principale du chat) — template + script (`pushMessage`, `workflowLabel`, `safeStepLabel`, etc.) | `ui/src/views/AssistantView.vue` | 216 clés `assistant.*` ajoutées (template + les 68 `pushMessage` + `workflowLabel`/`safeStepLabel`/`confidenceFor`/etc. du script) | Non → complet | **Haute** — vue la plus utilisée | **Fait, testé (09/09/2026, Claude)** |
| [x] U2 | Processus (attache, mode d'accès mémoire) | `ui/src/views/ProcessView.vue` | 22 clés `process.*` ajoutées (namespace existant réutilisé), intro + filtre + mode d'accès + actions driver kernel | Non → complet | **Haute** — première vue vue par un nouvel utilisateur (démarrage) | **Fait, testé (09/09/2026, Codex)** |
| [ ] U3 | Mode Expert — vue principale | `ui/src/views/ExpertView.vue` | ~150 (le plus gros fichier) | Non | Haute | Pas commencé |
| [ ] U3a | └ `AobSignaturePanel.vue` | `ui/src/components/expert/AobSignaturePanel.vue` | ~15 | Non (aide InfoDot oui, template non) | Moyenne | Pas commencé |
| [ ] U3b | └ `AutoDissectPanel.vue` | `ui/src/components/expert/AutoDissectPanel.vue` | ~1 | Non | Basse | Pas commencé |
| [ ] U3c | └ `CandidatePanel.vue` | `ui/src/components/expert/CandidatePanel.vue` | ~15 | Non (idem) | Moyenne | Pas commencé |
| [ ] U3d | └ `ExactScanPanel.vue` | `ui/src/components/expert/ExactScanPanel.vue` | ~10 | Non (idem) | Moyenne | Pas commencé |
| [ ] U3e | └ `FindWhatAccessesPanel.vue` | `ui/src/components/expert/FindWhatAccessesPanel.vue` | ~1 | Non | Basse | Pas commencé |
| [ ] U3f | └ `InjectionPanel.vue` | `ui/src/components/expert/InjectionPanel.vue` | ~3 | Non | Basse | Pas commencé |
| [ ] U3g | └ `NextScanPanel.vue` | `ui/src/components/expert/NextScanPanel.vue` | ~3 | Non | Basse | Pas commencé |
| [ ] U3h | └ `PointerChainScanPanel.vue` | `ui/src/components/expert/PointerChainScanPanel.vue` | ~3 | Non (idem) | Basse | Pas commencé |
| [ ] U3i | └ `PointerChainWatchPanel.vue` | `ui/src/components/expert/PointerChainWatchPanel.vue` | ~1 | Non | Basse | Pas commencé |
| [ ] U3j | └ `RegionPanel.vue` | `ui/src/components/expert/RegionPanel.vue` | ~3 | Non | Basse | Pas commencé |
| [ ] U3k | └ `SaveFilesPanel.vue` | `ui/src/components/expert/SaveFilesPanel.vue` | ~8 | Non | Basse-moyenne | Pas commencé |
| [ ] U3l | └ `SessionPanel.vue` | `ui/src/components/expert/SessionPanel.vue` | ~13 | Non | Basse-moyenne | Pas commencé |
| [ ] U3m | └ `UnknownScanPanel.vue` | `ui/src/components/expert/UnknownScanPanel.vue` | ~8 | Non (idem) | Basse-moyenne | Pas commencé |
| [ ] U3n | └ `WatchLivePanel.vue` | `ui/src/components/expert/WatchLivePanel.vue` | ~3 | Non | Basse | Pas commencé |
| [ ] U3o | └ `WritePanel.vue` | `ui/src/components/expert/WritePanel.vue` | ~3 | Non (idem) | Basse | Pas commencé |
| [ ] U3p | └ `GroupScanPanel.vue` | `ui/src/components/expert/GroupScanPanel.vue` | ~7 | Non | Basse | Pas commencé |
| [ ] U3q | └ `ActionLogPanel.vue` | `ui/src/components/expert/ActionLogPanel.vue` | ~1 | Non | Basse | Pas commencé |
| [ ] U4 | Investigation (carnet d'hypothèses, timeline) | `ui/src/views/InvestigationView.vue` | ~40 | Non | Haute | Pas commencé |
| [ ] U5 | Réglages | `ui/src/views/SettingsView.vue` | ~100 | Non | Haute | Pas commencé |
| [ ] U5a | └ `AssistantToolsPanel.vue` (sous-panneau Réglages) | `ui/src/components/settings/AssistantToolsPanel.vue` | ~10 | Non | Basse | Pas commencé |
| [ ] U6 | Trainer | `ui/src/views/TrainerView.vue` | ~20 | Non | Moyenne-haute | Pas commencé |
| [ ] U7 | Profils | `ui/src/views/ProfileView.vue` | ~10 | Non | Moyenne | Pas commencé |
| [ ] U8 | Scripting (Lua) | `ui/src/views/ScriptingView.vue` | ~15 | Non | Moyenne | Pas commencé |
| [ ] U9 | CLR Inspector | `ui/src/views/ClrInspectorView.vue` | ~25 | Non | Moyenne | Pas commencé |
| [ ] U10 | Mémoire (hex viewer) — reste | `ui/src/views/MemoryView.vue` | ~13 restants (18 `$t()` déjà en place) | Partiel | Basse | Pas commencé |
| [ ] U11 | Memory Heatmap | `ui/src/views/MemoryHeatmapView.vue` | ~20 | Non | Basse-moyenne | Pas commencé |
| [ ] U12 | Memory Timeline | `ui/src/views/MemoryTimelineView.vue` | ~15 | Non | Basse-moyenne | Pas commencé |
| [ ] U13 | Pattern Learning | `ui/src/views/PatternLearningView.vue` | ~20 | Non | Basse | Pas commencé |
| [ ] U14 | Speedhack | `ui/src/views/SpeedhackView.vue` | ~8 | Non | Basse-moyenne | Pas commencé |
| [ ] U15 | Réseau — reste | `ui/src/views/NetworkView.vue` | ~15 restants (51 `$t()` déjà en place) | Partiel | Basse | Pas commencé |
| [ ] U16 | Modules — reste (guide EDR notamment) | `ui/src/views/ModulesView.vue` | ~12 restants (41 `$t()` déjà en place) | Partiel | Basse | Pas commencé |
| [ ] U17 | WebView2 Inspector — reste | `ui/src/views/WebView2InspectorView.vue` | ~3 (bannière intro) | Partiel (19 `$t()` déjà en place) | Basse | Pas commencé |
| [ ] U18 | Lexique | `ui/src/views/LexiconView.vue` | 1 | Non | Basse | Pas commencé |
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

## Règle d'usage

Même règle que `docs/AI_CHAT_LOCALIZATION_ROADMAP.md` et `docs/REFACTOR_ROADMAP.md` : mettre à jour ce document (case cochée + date + nombre réel de chaînes traitées) à chaque candidat clos, journaliser dans `docs/PHASE_TRACKER.md`. Contrairement au chantier backend, la vérification se fait par `npm run type-check`/`npm run build` + contrôle visuel réel (pas de suite de tests unitaires côté frontend pour ces vues), donc un candidat n'est "fait" qu'après une vérification visuelle en direct (CDP screenshot ou lancement app), pas juste après une compilation propre.
