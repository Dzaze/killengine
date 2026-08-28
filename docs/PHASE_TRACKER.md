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
  - [x] Reliability Pass étape 1 validée en direct le 28/08/2026 (PHASE 185) : Freeze BP tient sous charge réelle (49 hits/49 rewrites) sur le tutoriel Cheat Engine (`Tutorial-x86_64.exe`), application tierce contrôlée choisie après échec sur Vampire Survivors (bruit temps réel trop chaotique pour le diff scan) et exclusion volontaire de Solitaire (compte propriétaire réel).
  - [ ] Reliability Pass étapes 3/4 restantes.
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
- [x] PHASE 185 (28/08/2026, Claude) - Reliability Pass étape 1 (Freeze BP sous charge réelle) validée + bug `setFreezeValue`/breakpoint corrigé, détail ci-dessous.

## Journal actif

- [x] PHASE 184 (28/08/2026, Codex) - Documentation : alléger `PHASE_TRACKER.md` sans perte d'historique
  - [x] **Quoi** : création de `docs/PHASE_TRACKER_HISTORY.md` contenant le contenu complet de l'ancien tracker avant allègement, puis remplacement de `docs/PHASE_TRACKER.md` par un tracker actif court : note de transfert visible, état courant, règles d'utilisation, validations ouvertes, phases récentes résumées et journal actif. `AGENTS.md` met aussi sa carte des fichiers `.md` à jour pour officialiser le nouveau fichier historique.
  - [x] **Pourquoi** : demande propriétaire explicite après constat que `PHASE_TRACKER.md` devenait trop gros pour rester ergonomique en contexte multi-agents. Objectif : réduire le risque de relire trop vite, de refaire un chantier déjà fait ou de créer des collisions, sans perdre le suivi historique.
  - [x] **Contraintes** : aucune information historique supprimée sans transfert ; l'ancien contenu complet est conservé dans `docs/PHASE_TRACKER_HISTORY.md`. Les futures phases continuent d'être consignées dans le tracker actif, avec renvoi vers des docs dédiés si nécessaire.
  - [x] **Comment vérifié** : `rg` confirme que `docs/PHASE_TRACKER_HISTORY.md` contient l'ancien contenu complet (PHASE 0, PHASE 120-A, PHASE 183) et que `docs/PHASE_TRACKER.md` pointe vers l'historique. Scan mojibake docs/AGENTS : seules les exceptions documentaires volontaires ressortent (`AGENTS.md` explique le motif et l'historique conserve les anciennes citations). `.\scripts\check-line-endings.ps1` exécuté : état historique du dépôt reporté, tracker actif désormais court et stable ; build non requis (documentation uniquement).

- [x] PHASE 185 (28/08/2026, Claude) - Reliability Pass étape 1 (Freeze BP sous charge réelle) + bug de désactivation de freeze corrigé
  - [x] **Quoi** : tentative live sur `Solitaire.exe` abandonnée avant tout scan (compte Microsoft réel du propriétaire, progression/défis du jour) ; bascule sur `VampireSurvivors.exe` également abandonnée après deux morts du personnage (pilotage clavier automatisé par l'agent trop imprécis dans ce jeu très dynamique) et échec du diff scan (Int32 puis Float32) à isoler la vie au milieu du bruit d'animations/particules temps réel. Sur suggestion du propriétaire, bascule sur `Tutorial-x86_64.exe` (fourni avec Cheat Engine, déjà installé) : Health trouvé par scan exact (100→96) en une passe, `freezeWithBreakpoint` posé sur l'adresse, tenu face à 49 clics réels sur "Hit me" (49 hits/49 rewrites rapportés par `stopBreakpointFreeze`), valeur affichée inchangée pendant toute la fenêtre. **Bug trouvé en testant la désactivation** : `setFreezeValue(adresse, ..., false)` répondait `success:true` mais ne faisait que retirer l'entrée du registre `m_freeze` et arrêter le timer de polling — sans jamais resynchroniser le vrai hardware breakpoint (DR0-3) posé par `freezeWithBreakpoint`, qui restait armé et continuait de réécrire la valeur gelée à chaque clic malgré la réponse "désactivé" trompeuse. Corrigé dans `apps/desktop/application_controller.cpp::setFreezeValue` : la branche de désactivation détecte maintenant si l'entrée retirée était en mode `HardwareBreakpoint` et appelle `restartBreakpointFreezeFromRegistry(...)` pour resynchroniser les registres de debug avec ce qui reste dans `m_freeze` (y compris le cas vide, qui arrête proprement `m_breakpointFreeze`).
  - [x] **Pourquoi** : feu vert explicite du propriétaire pour avancer sur les chantiers ouverts de `PHASE_TRACKER.md` ; seul reste ouvert le lot de validations manuelles (Reliability Pass, Third-Party Smoke Pass) nécessitant un rythme humain réel, d'où le choix collaboratif d'une cible plus calme après l'échec sur un jeu trop chaotique.
  - [x] **Comment vérifié** : entièrement en conditions réelles via le pipe d'automatisation + interactions réelles du propriétaire ("Hit me" cliqué à la main). Freeze BP posé/tenu/arrêté proprement une première fois (`stopBreakpointFreeze`, 49/49), puis reposé et désactivé une seconde fois via `setFreezeValue(..., false)` après rebuild avec le fix — confirmé par le propriétaire que les dégâts reprennent immédiatement après désactivation. Suite unitaire relancée après le fix : 247/247 verts (le fichier modifié n'est compilé que dans la cible `KillEngine`, hors périmètre des suites `killcore`/`killai`, donc ce passage confirme l'absence de régression ailleurs sans exercer directement le fix — la validation du fix lui-même est le test live ci-dessus). Aucun test automatisé dédié ajouté : `ApplicationController` n'a aucun harnais de test existant (couplage Qt + process réel attaché), cohérent avec le reste de ce niveau (validations toujours faites en direct, voir `docs/V1_REGRESSION_CHECKLIST.md`).
  - [x] **Reste ouvert** : Reliability Pass étapes 3 (AOB survit à un redémarrage) et 4 (chaîne de fallback pire cas), Authorized Third-Party Smoke Pass — non traités cette session.
