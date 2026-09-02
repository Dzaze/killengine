> **ATTENTION - Tracker actif allégé (nettoyage 29/08/2026, deuxième passe 31/08/2026 - PHASE 262, troisième passe 31/08/2026 - PHASE 273)**
> L'historique complet détaillé des phases validées a été transféré dans `docs/PHASE_TRACKER_HISTORY.md` pour ne rien perdre tout en gardant ce fichier lisible. Les agents doivent toujours lire `AGENTS.md` puis ce tracker actif avant d'agir ; consulter l'historique quand une ancienne phase, une décision passée ou un détail de validation est nécessaire.

# KillEngine Phase Tracker

Source of truth produit : `KILLENGINE_PROJECT_SPEC.md`
Historique détaillé : `docs/PHASE_TRACKER_HISTORY.md`
Roadmap power-up : `docs/POWER_UP_ROADMAP.md`
Roadmap refactorisation : `docs/REFACTOR_ROADMAP.md`

## État courant

- Phase 11 prototype, Phase 12 / polishing V1, Phase 13 (régression V1) : complètes et closes.
- PHASE 14A/14B, PHASE 17, PHASE 120-A/B/C/D, audit Arsenal 4 agents (PHASE 187 et ses corrections), la clarification `chat_memory_write`/`chat_memory_freeze`, PHASE 205 (branchement InvestigationView.vue) et PHASE 206 (Mode Automation, toggle + doc `docs/AUTOMATION_API.md`) : toutes closes et archivées en détail dans `docs/PHASE_TRACKER_HISTORY.md`.
- Dépôt git consolidé le 29/08/2026 : `main` remis à jour (fast-forward) sur l'unique branche de travail active, branches mortes supprimées.
- Refactor backend C1-C14 et stores frontend S1-S12 clos au 30/08/2026 — détails archivés dans `docs/PHASE_TRACKER_HISTORY.md`, roadmap cochée dans `docs/REFACTOR_ROADMAP.md`.
- **Carnet d'hypothèses (chantier "PHASE 120 v2", 30/08/2026)** : les 4 sous-phases 120-E/F/G/H sont closes — détail archivé. Ne pas relancer un nouveau chantier sur ce sujet sans accord explicite du propriétaire (règle posée le 27/08/2026, jamais levée).
- PHASES 241-261 (30-31/08/2026) closes et archivées : consensus multi-round Changed Pages (SC2 solarite), Page Guard multi-capture même PID, modules stealth (NtCreateThreadEx, process/DLL mask, anti-debug), corrections Assistant (négations Solitaire, pivot DLL/modules), vérifications visuelles live CLR Inspector/kernel driver.
- PHASES 263-270 (31/08/2026) closes et archivées : session de test terrain live du raisonnement Assistant (via pipe, `Solitaire.exe` réel) rejouant les corrections PHASE 263-269 (pivot DLL/modules sans/avec valeur, priorisation modules, suite conversationnelle, réduction explicite, Trace UI string et Changed Pages prioritaires sur négation) ; 3 bugs supplémentaires trouvés et corrigés en conditions réelles (`.exe` sans mot "module", régression `wantsChangedPages` sur conseil général, outils stealth oubliés du schéma LLM). Suite unitaire 291/291 à la clôture.
- PHASES 271-272 (31/08/2026) closes et archivées : décision propriétaire d'exposer tous les outils au LLM sans restriction de schéma (sécurité conservée via RiskGate frontend, pas via censure du schéma), uniformisation du nommage `getStealthStatus()`.
- **Investigation Solitaire XP (31/08/2026, toujours ouverte côté objectif final)** : recherche de la véritable adresse/source XP sur `Solitaire.exe`. Pistes natives épuisées (scan exact Int32/Float32/unknown sans résultat exploitable). Pistes WebView2/CDP épuisées aussi (voir chantier ci-dessous, clos) : la chaîne CDP fonctionne mais Solitaire n'expose que des WebViews de pub, pas le plateau de jeu. Reste ouvert : utiliser UWP-STATE-1 (snapshot avant/après, cf. ci-dessous) pour un vrai test terrain sur `LocalState`.
- **Chantier WebView2/CDP + UWP State Inspector (31/08-01/09/2026) : clos.** Détail complet archivé dans `docs/PHASE_TRACKER_HISTORY.md`. Résumé : 3ᵉ mode d'investigation KillEngine (JS/DOM via Chrome DevTools Protocol pour cibles hybrides natif+web), débloqué via Windows Device Portal + "Remote Tools for Microsoft Edge" après plusieurs pistes mortes (registre policy, port CDP direct bloqué par AppContainer, WDP natif). Chaîne connexion/lecture/écriture JS validée en live par le propriétaire (WEBVIEW-A à E), reconnaissance automatique du contexte JS avec baseline dynamique ajoutée et validée (WEBVIEW-F), diagnostic UWP State snapshot avant/après ajouté en réutilisant l'infrastructure `discover_save_files` existante (UWP-STATE-1). Environ 9 bugs de contrat backend/frontend trouvés et corrigés uniquement par usage réel (pipe d'automatisation + tests manuels du propriétaire) — aucun détectable par TypeScript seul, cf. [[feedback_verify_dont_trust_agent_build_claims]]. Deux nouvelles fonctionnalités Paramètres livrées (toggle debug CDP, diagnostic préparation WebView2). Solitaire XP lui-même reste non résolu (le plateau de jeu n'est ni en WebView2 ni trouvé en mémoire native) — piste suivante : UWP-STATE-1 sur `LocalState`, test terrain pas encore fait.

## Règle d'utilisation

- **Règle de branche KillEngine (décision propriétaire, 29/08/2026)** : travailler et committer directement sur `main`. Ne pas créer de branche `agent/...` dans ce dépôt.
- Ajouter les nouvelles phases ici, en bas de fichier, avec quoi/pourquoi/comment vérifié.
- Ne pas remettre tout l'historique dans ce fichier : dès qu'une phase est close, transférer le détail complet vers `docs/PHASE_TRACKER_HISTORY.md` et ne garder ici qu'un renvoi court.
- Ne jamais supprimer d'historique sans transfert explicite.
- Pour les gros détails d'une phase future, préférer un document dédié dans `docs/` puis garder ici un résumé avec lien.

## Validations restantes

- **UWP-STATE-1** : logique de diff avant/après validée par des données synthétiques via le pipe, mais pas encore par un vrai test terrain sur Solitaire (snapshot avant/gain XP/snapshot après/comparer) — reste à faire si quelqu'un reprend la piste Solitaire XP.
- **EXTMOD-1** : avant d'ouvrir un chantier générique "capacité de hook de logique de jeu", il faut differ `SC2_x64.exe` lui-même (pas les DLL de Wand) avant/après toggle pour confirmer si l'accroche réelle est un hook/redirection ou un patch statique — voir entrée "décision propriétaire" du 01/09/2026 ci-dessous.
- Sinon, aucune validation en attente : le chantier WebView2/CDP (WEBVIEW-A à F) et UWP-STATE-1 sont clos, voir "État courant" et `docs/PHASE_TRACKER_HISTORY.md` pour le détail.

## Journal actif


### Chantier clos — Inspection WebView2/JS (CDP) + UWP State Inspector (31/08-01/09/2026)

Démarré le 31/08/2026 suite à l'investigation Solitaire XP (voir "État courant"). Objectif atteint : 3ᵉ mode d'investigation KillEngine (inspection d'état JavaScript via Chrome DevTools Protocol pour cibles hybrides natif+web), plus un mode complémentaire d'investigation disque (snapshot avant/après fichiers UWP). **Détail complet (12 points de diagnostic WEBVIEW-A, implémentations WEBVIEW-B à F, 9 bugs de contrat trouvés en usage réel, UWP-STATE-1) archivé dans `docs/PHASE_TRACKER_HISTORY.md`** — ne pas re-déduire, consulter l'historique avant de reprendre ce sujet.

**Résumé des sous-phases closes** :
- **WEBVIEW-A** : accès CDP débloqué via Windows Device Portal + app Store "Remote Tools for Microsoft Edge" (le port CDP direct reste bloqué pour les apps UWP, restriction AppContainer définitive).
- **WEBVIEW-B** : client CDP core (`core/webview2/cdp_client.*`, `webview2_inspector.*`) avec discovery direct + fallback WDP.
- **WEBVIEW-C** : 7 outils Assistant/LLM (`ai/tool_registry.cpp`, `ai/ai_engine.cpp`, `ai/llama_runtime.cpp`), routage déterministe deux-étages.
- **WEBVIEW-D** : façade `ApplicationController` (Q_INVOKABLE + classification de risque).
- **WEBVIEW-E** : panneau dédié `WebView2InspectorView.vue` (calqué sur le CLR Inspector) + 2 fonctionnalités Paramètres (toggle debug CDP, diagnostic préparation WebView2). Validé en live par le propriétaire : lecture, écriture et retour de résultat structuré confirmés sur une vraie target Solitaire.
- **WEBVIEW-F** : reconnaissance automatique du contexte JS à la connexion (`probeWebView2GlobalScope`, baseline dynamique `about:blank`), validée en live — isole correctement le SDK publicitaire réel d'une page (`omid`, `videojs`, etc.) du bruit natif Chromium.
- **UWP-STATE-1** : snapshot avant/après fichiers UWP + diff (`compareProcessSaveFileSnapshots`), réutilisant l'infrastructure `discover_save_files` existante (PHASE 90+) plutôt que de la refaire. Logique de diff validée par données synthétiques ; test terrain réel sur Solitaire XP encore à faire (voir "Validations restantes").

**Conclusion pour Solitaire XP spécifiquement** : ni la mémoire native ni WebView2/CDP n'exposent le plateau de jeu (probablement natif XAML/DirectComposition) — reste ouvert via UWP-STATE-1 sur `LocalState`.

Règle de collision : si un nouveau sous-chantier WebView2/CDP ou UWP State démarre, poser les lanes de fichiers dans `docs/SALON.md` avant de coder (même règle que [[parallel_split_phase188_codex_pointer_map_deps]]).

### WEBVIEW-E — bug de contraste CSS trouvé en usage réel (01/09/2026, Claude)

**Quoi** : le propriétaire a signalé que le champ "Évaluer du JavaScript" était quasi illisible. Cause trouvée : `WebView2InspectorView.vue` utilisait dans tout son `<style>` des variables CSS **inventées qui n'existent nulle part dans le thème de l'app** (`--surface-1/2/3`, `--border-color`, `--primary`, `--text-muted`, `--success-bg`/`--success-text`, `--warning-bg`/`--warning-text`, `--error-bg`/`--error-text`) — le vrai thème global (`App.vue :root`) définit `--bg-primary/secondary/tertiary/accent`, `--text-primary/secondary/dim`, `--accent`, `--success`, `--warning`, `--error`, `--border`. Une variable CSS non définie rend la déclaration invalide, donc `background`/`color`/`border-color` ne s'appliquaient jamais sur la quasi-totalité du panneau (pas seulement le champ JS signalé).

**Corrigé** : toutes les occurrences remplacées par les vraies variables du thème (`--border-color`→`--border`, `--surface-1`→`--bg-secondary`, `--surface-2`→`--bg-tertiary`, `--surface-3`→`--bg-accent`, `--primary`→`--accent`, `--text-muted`→`--text-dim`, et les paires `*-bg`/`*-text` remplacées par `background: var(--bg-tertiary)` + `color: var(--success|warning|error)`, même convention que `ClrInspectorView.vue` déjà validée visuellement).

**Comment vérifié** : `npm run build` → 0 erreur. Grep de toutes les `var(--...)` du fichier après correction : seules les 11 vraies variables du thème restent. Pas de rebuild C++ nécessaire (l'app charge `ui/dist/index.html` en direct) — un simple relancement de `KillEngine.exe` suffit.

**Leçon** : encore un bug de contrat, cette fois entre le CSS d'une vue et le thème global — même famille de problème que les décalages de champs backend/frontend déjà documentés (cf. [[feedback_verify_dont_trust_agent_build_claims]]), sauf qu'ici rien ne pouvait le détecter avant un usage réel : une variable CSS invalide ne casse ni le build ni TypeScript, elle rend juste une déclaration silencieusement inopérante.

### WEBVIEW-F — bouton "Explorer" par globale détectée (01/09/2026, Claude)

**Quoi** : chaque entrée de la liste "Reconnaissance du contexte JS" (`customGlobals`) a maintenant un bouton "Explorer" qui pré-remplit le champ "Évaluer du JavaScript" avec une expression adaptée au type CDP de la globale (`buildProbeExpression()`, `WebView2InspectorView.vue`) : `window["nom"].toString().slice(0,500)` pour une fonction (affiche le code source), `JSON.stringify(Object.keys(window["nom"]))` pour un objet, ou lecture directe pour les valeurs primitives. Notation crochet systématique (pas `window.nom`) car certains noms observés ne sont pas des identifiants JS valides (ex: `"0"`, `"1"` pour des références de frame). Le champ est pré-rempli mais pas auto-évalué : l'utilisateur garde la main pour cliquer "Évaluer" et confirmer le RiskGate — pas d'exécution surprise.

**Pourquoi** : suite à une session de test manuel en direct sur la pub Solitaire (lecture/écriture confirmées, y compris un gag visuel de rotation CSS), le besoin de creuser une globale détectée sans retaper de JS à la main a été demandé explicitement (option B retenue face à une alternative "base de signatures connues").

**Comment vérifié** : `npm run build` → 0 erreur. Pas de test live de ce bouton précis à ce stade (nécessite de rouvrir l'app avec le CSS corrigé ci-dessus).

### UI-SHELL-1 — audit rendu premium des vues Vue (01/09/2026, Codex)

**Quoi** : retouche ciblée du CSS de `ui/src/App.vue` pour améliorer le panneau latéral global : fond plus profond, séparation droite plus lisible, relief discret, logo plus net, liens de navigation plus contrastés, état actif avec fond progressif et indicateur vertical accent orange/bleu. Audit statique des variables CSS dans toutes les vues/composants Vue, puis corrections de variables de thème invalides ou legacy : `--text` → `--text-primary` dans `ui/src/views/SettingsView.vue` et `ui/src/components/settings/AssistantToolsPanel.vue`, `--muted` → `--text-dim` dans `ui/src/views/ExpertView.vue`, `--surface-2`/`--border-color` legacy → `--bg-tertiary`/`--border` dans `ui/src/views/ClrInspectorView.vue`. Passe premium dédiée sur `ui/src/views/WebView2InspectorView.vue` : sections plus profondes, badges statut en pilule, boutons avec hover/focus plus nets, targets sélectionnées avec indicateur vertical, inputs/résultats plus contrastés, responsive compact. Aucun contrat backend/frontend modifié.

**Pourquoi** : retour propriétaire sur capture live : le contraste du panneau latéral gauche n'était "pas foufou" et pouvait être plus stylé/premium. Objectif étendu ensuite à une vérification globale des pages `.vue` : éviter les variables CSS silencieusement invalides et harmoniser les finitions visuelles avec le thème réel de KillEngine.

**Comment vérifié** : captures visuelles des 14 vues principales (`Assistant`, `Processus`, `Investigation`, `Mémoire`, `CLR`, `WebView2`, `Lua`, `Profils`, `Trainer`, `Speedhack`, `Réseau`, `Expert`, `Lexique`, `Paramètres`) via Vite local + Edge headless/CDP, planche contact inspectée manuellement ; capture WebView2 régénérée après la passe dédiée et inspectée. Script d'audit CSS : plus aucune variable `var(--...)` sans fallback ne référence une variable absente du thème. `cd ui && npm run type-check` → OK. `cd ui && npm run build` → OK (154 modules transformés, warning Vite pré-existant sur la taille du chunk JS > 500 kB). `git diff --check` sur les fichiers touchés → OK, seuls les avertissements Git LF/CRLF habituels.

### DOCS-SALON-1 — nettoyage du salon IA (01/09/2026, Codex)

**Quoi** : `docs/SALON.md` a été compacté pour redevenir une salle de stratégie lisible : objectifs/règles, format d'idée, lanes actives réelles, état récent à ne pas refaire, stratégies ouvertes (Solitaire XP, Investigation Router, UWP State/Save File Radar, UIA/OCR, ETW). Les longs échanges WebView2/UWP déjà consignés dans `docs/PHASE_TRACKER.md` et `docs/PHASE_TRACKER_HISTORY.md` ont été remplacés par des résumés et renvois.

**Pourquoi** : retour propriétaire : le salon doit surtout servir à discuter de stratégies, nouvelles technologies et outils à ajouter à KillEngine, pas devenir un second tracker verbeux ni un log de conversation complet.

**Comment vérifié** : relecture du salon nettoyé et `git diff --check` sur `docs/SALON.md`/`docs/PHASE_TRACKER.md`. Pas de build requis (docs-only).

### EXTMOD-1 — chantier identifié : Injected Module Differ / External Tool Profiler (01/09/2026, Codex)

**Quoi** : nouveau chantier produit identifié à partir d'une session terrain SC2 campagne/offline avec l'accord explicite du propriétaire : ajouter à KillEngine une capacité d'investigation passive des modifications faites par un outil externe autorisé sur une cible locale. Nom de travail : **Injected Module Differ** / **External Tool Profiler**. Le prototype manuel a comparé `SC2_x64.exe` avant/après activation du toggle `Invincibilité` dans Wand : modules chargés, protections mémoire, hash par page des DLL injectées, puis diff byte par byte sur les pages candidates. Détail stratégique et offsets conservés dans `docs/SALON.md` section "External Modifier Profiler / SC2 Campagne".

**Pourquoi** : après les limites rencontrées sur Solitaire XP (valeur affichée introuvable en mémoire native et plateau absent de WebView2), la session SC2 donne une piste plus générale pour KillEngine : quand une valeur ou un effet n'est pas trouvable par scan brut, observer les changements structurels autour de la cible (DLL injectées, pages XRW, protections, patches, stimulus ON/OFF) peut révéler la couche réellement responsable. Sur SC2, Wand ne se contente pas d'écrire une valeur externe : il injecte au moins `TrainerLibPlugin_x64.dll`, `Trainer_49560_b593cf46cc.dll` et `we-graphics-hook64.dll` dans `SC2_x64.exe`.

**Preuves terrain** : avant activation, SC2 avait 120 modules et aucun module Wand évident ; après activation `Invincibilité`, SC2 passe à 132 modules, charge les DLL ci-dessus, crée de nouvelles pages `IMAGE XRW` (~16.68 Mo) et augmente les régions exécutables privées. Désactiver le toggle ne décharge pas les DLL : le OFF agit probablement sur un état interne ou un patch déjà injecté. Diff OFF->ON : 258 pages changées au sens large mais seulement 3 pages changées en contenu. Diff `ON après dégât absorbé` vs `OFF après mort` : deltas byte-level courts sur 8 pages, dont `Trainer_49560_b593cf46cc.dll+0x7A000`, `+0x7B000`, `+0x20000`, `+0x81000` et `TrainerLibPlugin_x64.dll+0x1BF000`.

**Prototype minimal proposé** : dans une future phase dédiée, créer un outil KillEngine qui capture deux scénarios nommés (`baseline`, `toggle on`, `stimulus`) et produit automatiquement : diff de modules, diff de protections mémoire, détection des nouvelles pages RX/RWX/XRW, hash par page, byte-diff borné, classement "module injecté / page code / page data / bruit compteur". Le mode doit rester explicite et réservé aux cibles locales/autorisées/offline ; pas de contournement anti-cheat, pas d'usage multijoueur.

### SC2-LUA-1 — Wrappers Lua et documentation pour SC2 en mode stealth (02/09/2026, Claude)

**Quoi** : création d'une couche d'abstraction Lua pour le mode stealth SC2 et documentation des stratégies alternatives de scan pour les minéraux.

**Fichiers créés** :
- `scripts/killengine.lua` : wrappers `apply_stealth()`, `restore_stealth()`, `get_stealth_status()`, `kernel_write_value()`, `changed_pages_session_*()`
- `scripts/lua_examples/sc2_minerals_stealth.lua` : exemple complet de script SC2 avec mode stealth actif
- `docs/SC2_MINERALS_STRATEGIES.md` : guide des 7 stratégies alternatives au scan classique

**Stratégies documentées** (approche sans debugger) :
1. **Mode Stealth** : anti-debug + masquage processus/DLL via `apply_stealth("sc2")`
2. **Scan Multi-Type** : Int32, Float32, Int32×100, ×4096, ×65536 en une passe
3. **Scan Chiffré (XOR)** : quand les valeurs semblent aléatoires
4. **Trace UI String** : partir de l'affichage texte pour remonter à la source numérique
5. **Changed Pages Consensus** : session multi-rounds pour éliminer les copies UI volatiles
6. **Écriture via Driver Kernel** : plus discrète que WriteProcessMemory
7. **Freeze Logiciel** : polling à 16ms au lieu de hardware breakpoint (détectable)

**Pourquoi** : SC2 détecte les debuggers et les injections DLL. L'utilisateur demande une approche par pipe Lua qui évite d'attacher un debugger tout en restant fonctionnelle pour modifier les minéraux.

**Comment vérifié** : `npm run build` → OK. Les wrappers Lua sont des bindings vers les méthodes `Q_INVOKABLE` existantes d'`ApplicationController` (déjà validées en PHASE 253-261). La documentation couvre les pièges connus (valeur ×100, copies UI, Warden).

**Usage** :
```bash
# Lancer le script depuis KillEngine (Scripting view) ou ligne de commande
lua scripts/lua_examples/sc2_minerals_stealth.lua <pid_sc2> 50 9999
```

**Note** : Le mode stealth masque les signatures connues mais n'élimine pas totalement le risque de détection. Préférer les sessions courtes et éviter les écritures trop fréquentes.

**Comment vérifié** : validation live sur la machine du propriétaire, SC2 campagne/offline lancé, KillEngine démarré avec `KILLENGINE_AUTOMATION_PIPE=1`, `ping` OK, `attachProcess(19172)` OK. Relevés faits passivement par PowerShell (`Get-Process`, enumeration modules, `VirtualQueryEx`, `ReadProcessMemory` borné aux DLL injectées), puis consignés dans `docs/SALON.md`. Pas de build requis ici : documentation/planification uniquement, aucune modification de code.

### EXTMOD-1 — décision propriétaire : valider le mécanisme avant d'ouvrir le chantier générique (01/09/2026, Claude)

**Quoi** : objectif reformulé explicitement par le propriétaire — ce chantier ne vise pas à reproduire l'effet Wand sur SC2 spécifiquement, mais à identifier **ce qui manque à KillEngine de manière générique** par rapport à ce que font les trainers tiers pro (type Wand/WeMod). Question posée : est-ce que la découverte EXTMOD-1 justifie déjà d'ouvrir ce chantier générique ?

**Réponse/décision** : pas encore — il manque une preuve. Le byte-diff fait jusqu'ici (voir entrée EXTMOD-1 ci-dessus et `docs/SALON.md`) ne porte que sur l'intérieur des DLL injectées par Wand (son propre état interne qui change quand le toggle est OFF). Ça montre que Wand ne se contente pas d'un freeze de valeur statique (que KillEngine sait déjà faire), mais ça ne prouve pas encore *comment* l'effet s'accroche réellement dans `SC2_x64.exe` (hook de fonction/redirection vs patch statique classique). KillEngine dispose déjà d'un moteur de hook (MinHook, réutilisé pour le speedhack Mono, cf. `docs/PHASE_TRACKER_HISTORY.md` PHASE 14B) mais il n'est câblé que pour ce cas précis, pas exposé comme capacité générique.

**Prochaine étape validée avant tout chantier générique** : nouvelle session terrain SC2/Wand — cette fois differ `SC2_x64.exe` lui-même (pas les DLL de Wand) avant/après activation du toggle, pour confirmer si le point d'accroche réel est un hook de fonction/redirection dans le jeu ou un patch statique classique. Si hook confirmé → ouvrir le chantier générique "capacité de hook de logique de jeu" en plus du freeze/patch de valeur existant. Si patch statique → le gap est peut-être ailleurs (base de données d'offsets par jeu, pas la technique).

**Comment vérifier** : à faire — session live, propriétaire + agent disponible, cible `SC2_x64.exe` campagne/offline, comparer pages code/data de SC2 lui-même avant/après toggle Wand (pas seulement les DLL injectées cette fois).

### EXTMOD-1 — prototype backend `ExternalToolProfiler` livré et live-vérifié (01/09/2026, Claude)

**Quoi** : pendant que Wand était indisponible (limite d'usage atteinte), construction du moteur générique proposé dans l'entrée EXTMOD-1 ci-dessus. Nouveau manager `killengine::ExternalToolProfiler` (`apps/desktop/external_tool_profiler.h/.cpp`), branché sur `ApplicationController` comme les autres managers (`m_codePatchManager`, etc.), 4 nouvelles méthodes `Q_INVOKABLE` :
- `captureProfilerCheckpoint(label, options)` — capture nommée de l'état structurel du process attaché : modules (`ProcessEnumerator::enumerateModules`) + carte mémoire (`MemoryMap::snapshot`, protections/types) + hash de contenu par région (budget borné, `maxHashBytesMb`, défaut 64 Mo). Option `moduleName` pour scoper aux régions d'un module précis (évite de tout hasher sur un gros process).
- `getProfilerDiff(labelA, labelB, options)` — diff entre deux checkpoints : modules ajoutés/retirés, régions ajoutées/retirées/changées (protection, type, ou hash de contenu), avec classification (`injected_module_page`, `new_executable_writable_page`, `code_page_changed`, `image_data_page_changed`, `private_page_changed`, `other`).
- `listProfilerCheckpoints()` / `clearProfilerSession()`.
- `clearSessionState()` appelé automatiquement à chaque `attachProcess` (comme les autres managers) pour ne pas mélanger les checkpoints de deux cibles différentes.

Nommage volontairement générique (pas de vocabulaire SC2/Wand dans le code produit), conforme à la règle [[feedback_tools_must_stay_generic]] déjà appliquée ailleurs dans KillEngine.

**Pourquoi** : c'est le prototype minimal proposé dans l'entrée EXTMOD-1 initiale — réutilise l'infra existante (`ProcessEnumerator`, `MemoryMap`, `MemoryReader`, même hash FNV-1a que `display_string_investigator.cpp`) plutôt que de la refaire, dans l'esprit de [[phase189_clr_struct_nesting_and_doc_staleness]] et des autres réutilisations déjà documentées.

**Comment vérifié** : build via `scripts\build.ps1` (nécessaire — un `ninja` lancé directement hors `vcvars64.bat` échoue avec des erreurs `type_traits`/`cstdint` introuvables ; attention aussi à ne pas utiliser un `ninja.exe` trouvé au hasard dans le PATH, celui du projet est `C:\Python313\Scripts\ninja.exe`, cf. `CMAKE_MAKE_PROGRAM` dans `build/CMakeCache.txt`). Build réussi (`KillEngine.exe` relié, tests unitaires aussi compilés). Puis validation live réelle via le pipe automation (pas seulement un build propre, cf. [[feedback_verify_dont_trust_agent_build_claims]]) : `KillEngine.exe` lancé avec `KILLENGINE_AUTOMATION_PIPE=1`, attaché à `Notepad.exe` (cible synthétique, PID réel trouvé via `getProcesses` — le PID retourné par `Start-Process -PassThru` est un launcher, pas le vrai process UWP). Séquence testée avec succès : deux checkpoints identiques → diff vide (0 faux positif) ; déclenchement réel d'un changement (Ctrl+O, dialogue Ouvrir) → diff détecte correctement 2 régions ajoutées, 2 retirées, 36 changées de contenu, 0 module ajouté (cohérent : le dialogue de la version UWP moderne de Notepad ne charge pas de nouvelle DLL dans le process, juste de la mémoire de tas/UI) ; `listProfilerCheckpoints` retourne les 4 labels attendus ; ré-`attachProcess` sur un autre PID (`explorer.exe`) vide bien la session (`checkpoints: []`).

**Limites connues du prototype, à traiter avant usage terrain SC2** : classification actuelle basique (une seule bucket "other" pour tout ce qui n'est ni module injecté ni page RWX ni image) — suffisant pour distinguer "rien de suspect" de "signal à regarder", mais pas encore un classement fin bruit/compteur/stable. Pas de byte-diff intégré (reste composé avec `dumpMemoryRegion` existant en aval, volontairement — pas de duplication). Le matching de région par `baseAddress` exact peut rater un cas où Windows fusionne/scinde des régions entre deux captures (accepté comme limite connue, pas rencontré dans ce test).

**Prochaine étape** : reprendre le protocole SC2/Wand (ressources illimitées, cf. discussion propriétaire) dès que l'accès Wand revient, cette fois avec `captureProfilerCheckpoint`/`getProfilerDiff` au lieu du script PowerShell manuel utilisé pour la découverte initiale.

### Fix scan Auto — variantes scalées manquantes (x100/x1000/x4096/x65536), live-vérifié sur SC2 réel (01-02/09/2026, Codex puis Claude)

**Quoi** : pendant un test terrain sur une partie SC2 privée (capture de ressources), le propriétaire a remarqué que le mode Auto du scan exact multi-type ne cherchait pas les représentations scalées d'une valeur — seulement quelques types bruts figés (`UInt16, Int32, UInt32, Int64, Float32, Float64`), sans jamais tester qu'un jeu puisse stocker en mémoire une valeur multipliée par un facteur fixe (`x10, x100, x1000, x4096, x65536` — courant pour les compteurs à virgule fixe). Codex a corrigé `smartAutoScanVariants` (`apps/desktop/scanning_core_manager.cpp:374`) pour déléguer entièrement à `killcore::generateScanVariants` (déjà utilisé ailleurs) au lieu d'une liste de types codée en dur — ça ajoute d'un coup la couverture Int16/Int8/UInt8 manquante et les variantes scalées. `SC2.md` a aussi été mis à jour avec une section dédiée expliquant le phénomène (ex: `135` affiché peut être stocké `552960` = `135*4096`) et une limite encore ouverte : le workflow **Unknown Auto** (scan différentiel sans valeur de départ) n'est lui pas encore scale-aware — ne sait pas traduire un delta affiché (`+7`) en delta brut scalé (`+28672` pour x4096). Ce point reste à traiter séparément.

**Pourquoi ce test** : Codex a été bloqué par un quota d'usage 5h juste après le fix, avant de pouvoir revalider en conditions réelles. Claude a repris le retest live (contexte/quota séparé) pendant que Codex était indisponible.

**Comment vérifié** : build via `scripts\build.ps1` (déjà relinké à 00:12:45 avant même la reprise). KillEngine relancé avec `KILLENGINE_AUTOMATION_PIPE=1`, attaché à `SC2_x64.exe` PID 24152 (partie privée en cours, campagne/offline). `startExactScanMultiType("1199","Auto")` sur la vraie mémoire SC2 (minerai affiché = 1199) → 10898 candidats, avec des `variantLabel` scalés bien présents dans les résultats (ex: `"Int32 x65536"`), confirmant que le trou signalé est comblé sur cible réelle, pas seulement en théorie. Relecture du code de `nextScan`/`targetBytesForCandidate` (`scanning_core_manager.cpp:394`) : confirme que le narrowing (`nextScan`) est lui aussi scale-aware — il régénère les bytes cibles pour chaque candidat selon son propre `variantLabel` stocké, pas juste le scan initial. Tentative de narrowing complet (1199→1099) non poursuivie jusqu'au bout (0 survivant sur le premier round `exact`, attendu vu le volume de faux positifs d'un premier passage multi-type/multi-échelle sur 2 Go — pas un signe de bug) ; propriétaire a choisi d'arrêter là, le fix étant déjà démontré sur cible réelle.

### Bug trouvé et corrigé — `writeMemoryHex` réutilisait le handle ReadOnly de l'attach (02/09/2026, Claude)

**Quoi** : pendant le test terrain SC2 (narrowing minerai via Changed Pages, cf. ci-dessus), écriture de test `1099` via `writeMemoryHex` sur 3 candidats → échec sur les 3 avec `WriteProcessMemory failed ... error=5` (`ERROR_ACCESS_DENIED`). Cause trouvée dans le code : `ApplicationController::writeMemoryHex` (`apps/desktop/application_controller.cpp:3366`, avant fix) instanciait `killcore::MemoryWriter` directement sur `m_handle` — le handle partagé ouvert par `attachProcess` avec `killcore::ProcessAccess::ReadOnly` (voir `application_controller.cpp:2335`), qui n'a ni `PROCESS_VM_WRITE` ni `PROCESS_VM_OPERATION`. Tous les autres chemins d'écriture du moteur (`write_freeze_core_manager.cpp`, `freeze_hotkey_overlay_manager.cpp`) ouvrent au contraire leur propre handle `ProcessAccess::ReadWrite` à la demande — seul `writeMemoryHex` avait ce bug, confirmé en testant `writeMemoryValue` (chemin correct) sur la même adresse au même moment : succès immédiat (`verified: true`), prouvant que ce n'était pas une protection côté SC2 mais bien le handle ReadOnly réutilisé à tort.

**Corrigé** : `writeMemoryHex` ouvre maintenant son propre `killcore::ProcessHandle` en `ReadWrite` avant d'écrire, même pattern que `write_freeze_core_manager.cpp:165`.

**Comment vérifié** : build via `scripts\build.ps1` — premier essai échoué (`LNK1104: impossible d'ouvrir bin\KillEngine.exe`, le process attaché à SC2 tenait encore le fichier verrouillé, cf. [[feedback_check_binary_staleness_before_blaming_code]]), `KillEngine.exe` arrêté puis rebuild réussi. Pas encore re-testé en live sur `writeMemoryHex` lui-même après ce fix (le test SC2 s'est arrêté avant) — à revalider à la prochaine session si `writeMemoryHex` est réutilisé.

### SC2-UNKNOWN-1 — variantes `x4096`/`x65536` dans l'Auto exact (01/09/2026, Codex)

**Quoi** : correction ciblée de `apps/desktop/scanning_core_manager.cpp` après remarque du propriétaire pendant le test terrain SC2 : le générateur générique `core/scanner/value_variants.*` contient déjà les variantes `x10`, `x100`, `x1000`, `x4096`, `x65536`, et le scan exact multi-type `Auto` les utilisait bien, mais le chemin `SmartAuto` utilisé par l'Assistant avait encore une liste rapide maison de types bruts (`UInt16`, `Int32`, `UInt32`, `Int64`, `Float32`, `Float64`). `SmartAuto` est maintenant aligné sur `generateScanVariants(value, Int32, false)` pour inclure les mêmes variantes que l'Auto produit.

**Pourquoi** : pour SC2, une ressource affichée peut être stockée comme fixed-point (`135 * 4096 = 552960`, par exemple). Le libellé "Auto" doit donc vouloir dire "types + représentations utiles", pas "quelques types bruts". Sans cette correction, une demande Assistant avec valeur connue pouvait rater un candidat `x4096`/`x65536` alors que le moteur savait déjà le chercher par ailleurs.

**Reste ouvert** : le workflow `Unknown Auto` ne passe toujours pas par `generateScanVariants()` car il compare un snapshot avant/après sans valeur affichée cible ; il boucle seulement sur des `ValueType` bruts. Une passe Unknown `Increased` peut conserver un fixed-point parce que le brut augmente aussi, mais KillEngine ne sait pas encore exploiter directement le delta affiché (`+7`) comme delta brut (`+28672` pour `x4096`) ni labelliser automatiquement les survivants. Futur chantier possible : refine Unknown delta-aware basé sur `previousDisplayedValue`, `currentDisplayedValue` et les variantes de `value_variants`.

**Comment vérifié** : lecture ciblée de `core/scanner/value_variants.h/.cpp` (variantes `x4096`/`x65536` présentes et testées par `tests/unit/test_value_variants.cpp`), `apps/desktop/scanning_core_manager.cpp` (`startExactScanMultiType()` et `smartAutoScanVariants()`), et `core/snapshot/snapshot_store.cpp` (`SnapshotStore::compare()` mono-`ValueType`, changed/unchanged/increased/decreased seulement). Pas de build lancé volontairement pendant la session multi-agent en cours ; vérification légère par `git diff --check` et scan mojibake sur les fichiers touchés.

### Validation globale avant commit — Build + unit tests (02/09/2026, Codex)

**Quoi** : validation finale du lot de changements en attente avant commit sur `main` : profiler externe, wrappers/scripts SC2, retouches UI, correction `SmartAuto` scale-aware et correction `writeMemoryHex`.

**Pourquoi** : demande propriétaire explicite : "relance un build et test puis comit tout", reprise après disponibilité de Wand et avant consolidation Git.

**Comment vérifié** : `.\scripts\build.ps1` → OK (`Build successful!`, `build\bin\KillEngine.exe` généré). `.\build\bin\killengine_unit_tests.exe` → OK, 291 tests passés sur 43 suites.
