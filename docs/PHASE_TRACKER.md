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
