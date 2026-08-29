> **ATTENTION - Tracker actif allégé (nettoyage 29/08/2026)**
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
- Refactor C1/C2/C6/C7 (Codex, en cours sur C3/C4/C5) et S1-S6 + `confirmRiskAction` (Claude — TOUTES les fondations partagées côté store sont désormais extraites) clos le 29/08/2026 — extraction progressive sans chevauchement de fichiers entre agents : détail archivé dans `docs/PHASE_TRACKER_HISTORY.md`, roadmap cochée dans `docs/REFACTOR_ROADMAP.md`. Plus aucun blocage architectural côté store pour S7-S12 (`riskGate.ts`, PHASE 216) ; côté C++, reste `ScanStateAccess`/`AutoWriteStateAccess` avant C8-C11.
- Plus aucun chantier ouvert au 29/08/2026 - voir "Chantiers ouverts" ci-dessous.

## Règle d'utilisation

- **Règle de branche KillEngine (décision propriétaire, 29/08/2026)** : travailler et committer directement sur `main`. Ne pas créer de branche `agent/...` dans ce dépôt, même si `AGENTS.md` mentionne encore l'ancienne règle multi-branches ; la consolidation du dépôt a volontairement ramené le flux multi-agents à une seule branche pour éviter qu'un autre agent commette par erreur hors de `main`.
- Ajouter les nouvelles phases ici, en bas de fichier, avec quoi/pourquoi/comment vérifié.
- Ne pas remettre tout l'historique dans ce fichier : dès qu'une phase est close, transférer le détail complet vers `docs/PHASE_TRACKER_HISTORY.md` et ne garder ici qu'un renvoi court.
- Ne jamais supprimer d'historique sans transfert explicite.
- Pour les gros détails d'une phase future, préférer un document dédié dans `docs/` puis garder ici un résumé avec lien.

## Chantiers ouverts / validations restantes

- [ ] Chantier permanent - Refactorisation incrémentale d'`application_controller.cpp`/`app.ts`
  - Voir `docs/REFACTOR_ROADMAP.md` pour la liste des candidats d'extraction (chacun délimité pour être pris par un seul agent, sans chevauchement avec les autres).
  - **Ne jamais bloquer une phase produit pour ce chantier** : extraire seulement quand une phase future touche déjà la zone concernée pour une autre raison. Ne pas ouvrir de session dédiée uniquement pour ça sauf demande explicite du propriétaire.
  - Ordre : candidats à couplage bas d'abord (indépendants), puis les fondations partagées (`addActionLog`/`addInvestigationStep`/`confirmRiskAction` côté store, interface d'accès scan/write côté C++), le dispatch chat/IA (`startSmartSearch` et son cluster) en tout dernier et jamais réparti entre deux agents en parallèle.

## Journal actif

- [x] PHASE 207 - Refactor C1 extrait et archivé dans `docs/PHASE_TRACKER_HISTORY.md`.
- [x] PHASE 208 - Refactor S6 extrait et archivé dans `docs/PHASE_TRACKER_HISTORY.md`.
- [x] PHASE 209 - Refactor C2 extrait et archivé dans `docs/PHASE_TRACKER_HISTORY.md`.
- [x] PHASE 210 - Refactor S5 extrait et archivé dans `docs/PHASE_TRACKER_HISTORY.md`.
- [x] PHASE 211 - Refactor S1 extrait et archivé dans `docs/PHASE_TRACKER_HISTORY.md`.
- [x] PHASE 213 - Refactor S2 extrait et archivé dans `docs/PHASE_TRACKER_HISTORY.md`.
- [x] PHASE 214 - Refactor S3/S4 extraits et archivés dans `docs/PHASE_TRACKER_HISTORY.md`. Tous les candidats store à faible couplage (S1-S6) sont désormais clos.
- [x] PHASE 212 - Refactor C6 extrait et archivé dans `docs/PHASE_TRACKER_HISTORY.md`.
- [x] PHASE 215 - Refactor C7 extrait et archivé dans `docs/PHASE_TRACKER_HISTORY.md`.
- [x] PHASE 216 - Refactor RiskGate (`confirmRiskAction`) extrait et archivé dans `docs/PHASE_TRACKER_HISTORY.md`. Dernière fondation partagée côté store close.
