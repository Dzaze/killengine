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
- Refactor backend C1-C14 et stores frontend S1-S12 clos au 30/08/2026 — détails archivés dans `docs/PHASE_TRACKER_HISTORY.md`, roadmap cochée dans `docs/REFACTOR_ROADMAP.md`. `app.ts` et `ApplicationController` n'ont plus de gros candidat refactor ouvert identifié ; repartir d'un inventaire frais avant toute nouvelle vague.
- Plus aucun chantier de refactorisation ouvert au 30/08/2026 - restent seulement les validations/doc résiduelles listées ci-dessous.

## Règle d'utilisation

- **Règle de branche KillEngine (décision propriétaire, 29/08/2026)** : travailler et committer directement sur `main`. Ne pas créer de branche `agent/...` dans ce dépôt, même si `AGENTS.md` mentionne encore l'ancienne règle multi-branches ; la consolidation du dépôt a volontairement ramené le flux multi-agents à une seule branche pour éviter qu'un autre agent commette par erreur hors de `main`.
- Ajouter les nouvelles phases ici, en bas de fichier, avec quoi/pourquoi/comment vérifié.
- Ne pas remettre tout l'historique dans ce fichier : dès qu'une phase est close, transférer le détail complet vers `docs/PHASE_TRACKER_HISTORY.md` et ne garder ici qu'un renvoi court.
- Ne jamais supprimer d'historique sans transfert explicite.
- Pour les gros détails d'une phase future, préférer un document dédié dans `docs/` puis garder ici un résumé avec lien.

## Chantiers ouverts / validations restantes

- [x] Chantier permanent - Refactorisation incrémentale d'`application_controller.cpp`/`app.ts` close pour la vague C1-C14/S1-S12
  - Voir `docs/REFACTOR_ROADMAP.md` : tous les candidats identifiés sont cochés. Ne rouvrir un chantier refactor qu'après nouvel inventaire concret du code.
- [ ] Refactor entièrement clos (backend : C1-C14 depuis PHASE 234 ; frontend : S1-S12 depuis PHASE 236) : faire un inventaire exhaustif de la forme de réponse (`QVariantMap`) de **toutes** les méthodes `Q_INVOKABLE` dans `docs/AUTOMATION_API.md` (ou un doc dédié référencé depuis lui) — décision explicite du propriétaire, PHASE 233 (30/08/2026). Volontairement **pas fait avant** : le faire pendant que la surface bouge encore ferait périmer la doc au fil du refactor (même piège que `docs/POWER_UP_ROADMAP.md`). D'ici là, la convention de recherche (`Select-String -Pattern 'result\["\w+"\]\s*='` sur le `.cpp` du manager concerné) reste le seul moyen fiable de connaître une forme de réponse — voir `docs/AUTOMATION_API.md`.
- [ ] Deux résidus mineurs "confort", jamais promus en PHASE ni explicitement classés "décision produit de ne pas faire" (audit produit/QA/doc, PHASE 236, 30/08/2026 — demande explicite du propriétaire de vérifier qu'aucun chantier produit/QA/doc ne restait non tracké après la clôture du refactor) :
  - `docs/POWER_UP_ROADMAP.md` section D (Analyseur de structures), point 3 : déduction automatique du delta d'offset entre deux instances de la même entité — proposé le 19/08/2026, jamais repris depuis ; l'utilisateur compare toujours deux dissections manuellement.
  - `docs/POWER_UP_ROADMAP.md` section K (Lua) : le décodeur JSON maison de `ke.decode_json` ne gère pas les échappements `\uXXXX` — jamais rencontré en pratique, jamais corrigé.
  - Ne bloque aucune phase produit : à prendre seulement si un futur chantier touche déjà cette zone, même règle que le chantier refactor ci-dessus.

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
- [x] PHASE 217 - Refactor C3/C4/C5 extraits et archivés dans `docs/PHASE_TRACKER_HISTORY.md`. Toute la première vague backend C1-C7 est désormais close.
- [x] PHASE 218 - Refactor S12 (Settings) extrait et archivé dans `docs/PHASE_TRACKER_HISTORY.md`. Travail solo (Codex a atteint sa limite quotidienne).
- [x] PHASE 219 - Refactor C11a (cycle de vie pipe d'automatisation) extrait et archivé dans `docs/PHASE_TRACKER_HISTORY.md`. C11 complet scindé en C11a (fait) / C11b (reste, plus entremêlé que prévu — attend C8/ScanStateAccess).
- [x] PHASE 220 - Refactor S9a (CRUD templates de structure + bookmarks workspace) extrait et archivé dans `docs/PHASE_TRACKER_HISTORY.md`. S9 complet scindé en S9a (fait) / S9b (reste, haut couplage avec S7/S8).
- [x] PHASE 221 - Refactor fondation `AutoWriteStateAccess` v1 extrait et archivé dans `docs/PHASE_TRACKER_HISTORY.md`.
- [x] PHASE 222 - CLR Inspector : lecture `ConcurrentQueue<T>` livrée et archivée dans `docs/PHASE_TRACKER_HISTORY.md`.
- [x] PHASE 223 - Refactor fondation `ScanStateAccess` v1 extrait et archivé dans `docs/PHASE_TRACKER_HISTORY.md`.
- [x] PHASE 224 - Refactor S11a (Scanning/Candidates) extrait et archivé dans `docs/PHASE_TRACKER_HISTORY.md`.
- [x] PHASE 225 - CLR Inspector : lecture `ConcurrentBag<T>` livrée et archivée dans `docs/PHASE_TRACKER_HISTORY.md`.
- [x] PHASE 226 - Refactor C9 `WriteFreezeCoreManager` extrait et archivé dans `docs/PHASE_TRACKER_HISTORY.md`.
- [x] PHASE 227 - CLR Inspector : setters à paramètre `struct` de 9+ octets livrés et archivés dans `docs/PHASE_TRACKER_HISTORY.md`.
- [x] PHASE 228 - Refactor C8 `ScanningCoreManager` extrait et archivé dans `docs/PHASE_TRACKER_HISTORY.md`.
- [x] PHASE 229 - Refactor S7 (Write/Freeze/Checkpoint) extrait et archivé dans `docs/PHASE_TRACKER_HISTORY.md`.
- [x] PHASE 230 - Refactor S8 (Trainer Features) extrait et archivé dans `docs/PHASE_TRACKER_HISTORY.md`.
- [x] PHASE 231 - Refactor C10 `ProfileManager` extrait et archivé dans `docs/PHASE_TRACKER_HISTORY.md`.
- [x] PHASE 232 - Refactor C11b `SettingsDiagnosticsManager` extrait et archivé dans `docs/PHASE_TRACKER_HISTORY.md`.
- [x] PHASE 233 - Automation API : formes de réponses profils + packaging doc + batterie pipe profils livrés et archivés dans `docs/PHASE_TRACKER_HISTORY.md`.
- [x] PHASE 234 - Refactor backend C12/C13/C14 `SmartSearchManager` extrait et archivé dans `docs/PHASE_TRACKER_HISTORY.md`.
- [x] PHASE 235 - Refactor S9b (reste de Profils/Workspace) extrait et archivé dans `docs/PHASE_TRACKER_HISTORY.md`.
- [x] PHASE 236 - Refactor S10 + S11b (Assistant / Smart Search / Candidate Actions) extrait et archivé dans `docs/PHASE_TRACKER_HISTORY.md`. Backend C1-C14 et stores S1-S12 sont désormais clos.
- [x] PHASE 237 - Nettoyage tracker actif : détails PHASE 221-236 transférés dans `docs/PHASE_TRACKER_HISTORY.md`, tracker réduit à des renvois courts. Vérification : relecture du tracker + recherche de l'entrée d'archive dans l'historique ; pas de build requis (documentation uniquement).
