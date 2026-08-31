> **ATTENTION - Tracker actif allégé (nettoyage 29/08/2026, deuxième passe 31/08/2026 - PHASE 262)**
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
- Aucune capacité manquante identifiée dans l'arsenal (convergence 4 agents indépendants).
- Refactor backend C1-C14 et stores frontend S1-S12 clos au 30/08/2026 — détails archivés dans `docs/PHASE_TRACKER_HISTORY.md`, roadmap cochée dans `docs/REFACTOR_ROADMAP.md`. `app.ts` et `ApplicationController` n'ont plus de gros candidat refactor ouvert identifié ; repartir d'un inventaire frais avant toute nouvelle vague.
- Plus aucun chantier de refactorisation ouvert au 30/08/2026 - restent seulement les validations/doc résiduelles listées ci-dessous.
- **Carnet d'hypothèses (chantier "PHASE 120 v2", 30/08/2026)** : les 4 sous-phases 120-E/F/G/H (moteur de pondération déterministe, UI, génération d'hypothèses par modèle local, validation terrain sur `KillEngineTestTarget`) sont closes — détail complet archivé dans `docs/PHASE_TRACKER_HISTORY.md`. Ne pas relancer un nouveau chantier sur ce sujet sans accord explicite du propriétaire (règle posée le 27/08/2026, jamais levée).
- PHASES 241-261 (30-31/08/2026) closes et archivées : consensus multi-round Changed Pages (SC2 solarite) + intégration auto-resolve/UI Expert/validatePageStability, Page Guard multi-capture même PID, modules stealth (NtCreateThreadEx, process/DLL mask, anti-debug) + wrapper `applyStealthMode`, corrections Assistant (négations Solitaire, pivot DLL/modules Bubble XP, `list_process_modules`, `exact_scan_module`, garde-fou SC2), vérifications visuelles live CLR Inspector/kernel driver, mises à jour doc (`KILLENGINE_CODE_MAP.md`, `POWER_UP_ROADMAP.md`). Détail complet dans `docs/PHASE_TRACKER_HISTORY.md`.

## Règle d'utilisation

- **Règle de branche KillEngine (décision propriétaire, 29/08/2026)** : travailler et committer directement sur `main`. Ne pas créer de branche `agent/...` dans ce dépôt, même si `AGENTS.md` mentionne encore l'ancienne règle multi-branches ; la consolidation du dépôt a volontairement ramené le flux multi-agents à une seule branche pour éviter qu'un autre agent commette par erreur hors de `main`.
- Ajouter les nouvelles phases ici, en bas de fichier, avec quoi/pourquoi/comment vérifié.
- Ne pas remettre tout l'historique dans ce fichier : dès qu'une phase est close, transférer le détail complet vers `docs/PHASE_TRACKER_HISTORY.md` et ne garder ici qu'un renvoi court.
- Ne jamais supprimer d'historique sans transfert explicite.
- Pour les gros détails d'une phase future, préférer un document dédié dans `docs/` puis garder ici un résumé avec lien.

## Validations restantes

Aucune. Les deux résidus produit/QA/doc relevés en PHASE 236 sont clos (delta d'offset Structure Analyzer en PHASE 239, échappements `\uXXXX` Lua en PHASE 240 — détails dans `docs/PHASE_TRACKER_HISTORY.md`, archive complémentaire PHASES 238-240).

## Journal actif

Aucune phase en cours. Le carnet d'hypothèses (120-E/F/G/H) et les PHASES 241-261 sont closes et archivées dans `docs/PHASE_TRACKER_HISTORY.md` (voir "État courant" ci-dessus pour le résumé, nettoyage PHASE 262 du 31/08/2026).
