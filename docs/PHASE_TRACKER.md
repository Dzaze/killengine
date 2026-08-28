> **ATTENTION - Tracker actif allégé (PHASE 184, 28/08/2026)**
> L'historique complet détaillé des phases validées a été transféré dans `docs/PHASE_TRACKER_HISTORY.md` pour ne rien perdre tout en gardant ce fichier lisible. Les agents doivent toujours lire `AGENTS.md` puis ce tracker actif avant d'agir ; consulter l'historique quand une ancienne phase, une décision passée ou un détail de validation est nécessaire.

# KillEngine Phase Tracker

Source of truth produit : `KILLENGINE_PROJECT_SPEC.md`
Historique détaillé : `docs/PHASE_TRACKER_HISTORY.md`
Roadmap power-up : `docs/POWER_UP_ROADMAP.md`

## État courant

- Phase 11 prototype : complète et revalidée via Assistant/profils.
- Phase 12 / polishing V1 : complète.
- Phase 13 : baseline améliorations largement livrée ; seules des validations manuelles restent ouvertes.
- PHASE 120-A : ouverte puis livrée en deux parties le 28/08/2026 (`docs/INVESTIGATION_PLAYBOOK.md` + câblage backend lecture seule). PHASE 120-B/C/D restent hors scope et nécessitent un nouvel accord explicite du propriétaire.
- Prochain focus récurrent : finir la passe de régression manuelle V1 sur une application tierce autorisée et les validations debugger restantes avant release candidate.

## Règle d'utilisation

- Ajouter les nouvelles phases ici, en bas de fichier, avec quoi/pourquoi/comment vérifié.
- Ne pas remettre tout l'historique dans ce fichier : s'il faut un détail ancien, pointer vers `docs/PHASE_TRACKER_HISTORY.md`.
- Ne jamais supprimer d'historique sans transfert explicite.
- Pour les gros détails d'une phase future, préférer un document dédié dans `docs/` puis garder ici un résumé avec lien.

## Chantiers ouverts / validations restantes

- [ ] PHASE 13 - Robustesse et validations restantes
  - [ ] Passe de régression manuelle V1 sur KillEngineTestTarget et une application tierce autorisée.
  - [x] Sections Assistant + Displayed vs Source Classifier closes le 27/08/2026.
  - [x] Reliability Pass étape 2 validée en direct le 27/08/2026 : pointer chain survit à un redémarrage complet sur `KillEngineTestTarget.exe`.
  - [ ] Reliability Pass étapes 1/3/4 restantes.
  - [ ] Authorized Third-Party Smoke Pass restant.

- [ ] PHASE 14 - Pointer Chains : validation manuelle sur application moderne tierce autorisée encore ouverte.

- [ ] PHASE 17 - Find What Writes / debugger expérimental
  - [x] Module hardware breakpoint, backend, UI Expert, async worker, annulation et garde-fous UX v1 livrés.
  - [ ] Validation manuelle debugger sur application tierce autorisée.
  - [ ] Validation manuelle prolongée avant généralisation.

- [ ] PHASE 120-B/C/D - Assistant enquête avancée
  - [x] PHASE 120-A : copilote d'enquête lecture seule livré (`docs/INVESTIGATION_PLAYBOOK.md`, `AIEngine::processQuery`, tests 247/247 au 28/08/2026).
  - [ ] PHASE 120-B : recommandations structurées avec boutons d'action confirmés, accord propriétaire requis.
  - [ ] PHASE 120-C : enchaînements semi-automatiques read-only, accord propriétaire requis.
  - [ ] PHASE 120-D : escalade debug/patch/write/freeze complète, accord propriétaire requis.
  - [ ] Branchement UI `InvestigationView.vue` à planifier seulement après décision produit explicite.

## Phases récentes résumées

- [x] PHASE 170 (28/08/2026, Codex) - Assistant : garde social pur côté `AIEngine::processQuery` pour éviter un démarrage IA local sur `salut`/`merci`.
- [x] PHASE 171 (28/08/2026) - UX : décision d'ajouter un contexte débutant sur chaque panneau sidebar.
- [x] PHASE 172 (28/08/2026, Cline) - UI : panneau `Lexique` alphabétique ajouté dans la sidebar.
- [x] PHASE 173 (28/08/2026, Claude) - UI : MVP `PanelIntro` sur CLR/Lua.
- [x] PHASE 174 (28/08/2026, Codex) - Assistant : garde social aussi côté `startSmartSearch`.
- [x] PHASE 175 (28/08/2026, Claude) - QA visuelle Lexique + PanelIntro CLR/Lua, aucun bug trouvé.
- [x] PHASE 176 (28/08/2026) - Smoke live du garde social rejoué avec succès, freeze dû à un binaire obsolète confirmé.
- [x] PHASE 177 (28/08/2026, Claude) - Plan de fusion : ne pas fusionner les deux classifieurs, extraire seulement les garde-fous textuels stateless.
- [x] PHASE 178 (28/08/2026, Codex) - Implémentation `ai/query_text_utils.*`, partage des prédicats entre `AIEngine` et `startSmartSearch`.
- [x] PHASE 179 (28/08/2026, Claude) - `PanelIntro` branché sur les 10 vues restantes.
- [x] PHASE 180 (28/08/2026, Codex) - Tests dédiés `query_text_utils`, 243/243 unitaires OK.
- [x] PHASE 181 (28/08/2026, Claude) - QA visuelle réelle des 12 vues sidebar, aucun bug trouvé.
- [x] PHASE 182 (28/08/2026, Cline) - Ménage tracker : fermeture de 3 cases stale.
- [x] PHASE 120-A (28/08/2026, Claude) - Création `docs/INVESTIGATION_PLAYBOOK.md`, playbook déclaratif, pas routeur.
- [x] PHASE 183 (28/08/2026, Codex) - Câblage backend lecture seule du playbook PHASE 120-A, tests 247/247 OK.

## Journal actif

- [x] PHASE 184 (28/08/2026, Codex) - Documentation : alléger `PHASE_TRACKER.md` sans perte d'historique
  - [x] **Quoi** : création de `docs/PHASE_TRACKER_HISTORY.md` contenant le contenu complet de l'ancien tracker avant allègement, puis remplacement de `docs/PHASE_TRACKER.md` par un tracker actif court : note de transfert visible, état courant, règles d'utilisation, validations ouvertes, phases récentes résumées et journal actif. `AGENTS.md` met aussi sa carte des fichiers `.md` à jour pour officialiser le nouveau fichier historique.
  - [x] **Pourquoi** : demande propriétaire explicite après constat que `PHASE_TRACKER.md` devenait trop gros pour rester ergonomique en contexte multi-agents. Objectif : réduire le risque de relire trop vite, de refaire un chantier déjà fait ou de créer des collisions, sans perdre le suivi historique.
  - [x] **Contraintes** : aucune information historique supprimée sans transfert ; l'ancien contenu complet est conservé dans `docs/PHASE_TRACKER_HISTORY.md`. Les futures phases continuent d'être consignées dans le tracker actif, avec renvoi vers des docs dédiés si nécessaire.
  - [x] **Comment vérifié** : `rg` confirme que `docs/PHASE_TRACKER_HISTORY.md` contient l'ancien contenu complet (PHASE 0, PHASE 120-A, PHASE 183) et que `docs/PHASE_TRACKER.md` pointe vers l'historique. Scan mojibake docs/AGENTS : seules les exceptions documentaires volontaires ressortent (`AGENTS.md` explique le motif et l'historique conserve les anciennes citations). `.\scripts\check-line-endings.ps1` exécuté : état historique du dépôt reporté, tracker actif désormais court et stable ; build non requis (documentation uniquement).
