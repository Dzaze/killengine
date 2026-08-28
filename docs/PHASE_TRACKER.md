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

## 🔎 Arsenal — ce qu'il manque (audit croisé 4 agents, 28/08/2026)

Le propriétaire a posé la même question à 4 agents indépendants (Claude, Codex, Cline/GLM 5.2, un 4ᵉ agent) : *"a-t-on tout l'arsenal nécessaire ?"* Convergence totale : **oui pour les capacités, non pour la validation terrain.** Aucun des 4 n'a trouvé de vraie capacité manquante. Détail des seuls écarts trouvés :

**Exclusions volontaires (décision produit déjà prise — ne pas reconstruire) :**
- DMA hardware (carte PCIe dédiée) — matériel non disponible ; chantier possible si acquis un jour.
- Lua embarqué in-process (vs shell-out actuel) — différé tant que rien ne prouve un vrai besoin.
- Remplacer Ghidra/IDA (désassembleur/décompilateur statique complet) — `docs/ULTIMATE_PRODUCT_GUIDELINE.md` fixe explicitement : ne pas reconstruire Ghidra, devenir un pont vers lui.

**Confort non bloquant, déjà documenté dans `docs/POWER_UP_ROADMAP.md` :**
- ~~Pont export/import d'artefacts vers Ghidra (AOB, offsets, notes, symboles) — 0% construit.~~ **Correction PHASE 193 : livré** (`core/profiles/ghidra_bridge.*`, export JSON + script Python Ghidra + import symboles JSON/CSV).
- ~~Persistance backend/profil des dépendances Trainer (`dependsOn`) — vit seulement dans le store frontend/localStorage.~~ **Correction PHASE 190 : livré** (`ProfileTarget::dependsOn`, `setProfileTargetDependencies`, sauvegarde par noms stables).
- ~~Export/import d'une map de pointeurs en texte partageable.~~ **Correction PHASE 190 : livré** (`ProfileStore::exportPointerMap`/`mergePointerMap`, UI Profil).
- ~~Quelques extensions CLR pointues (struct-dans-struct-dans-tableau en écriture, setters à paramètre `struct`, `ConcurrentDictionary`)~~ **Correction (PHASE 189, 28/08/2026) : toutes closes.** Ce point était basé sur un `docs/POWER_UP_ROADMAP.md` déjà périmé au moment de l'audit PHASE 187 — les 3 étaient déjà livrées (PHASE 79-81, 24/08/2026) ou vérifiées le jour même (struct-dans-struct-dans-tableau, PHASE 189). Reste réellement non couvert et non bloquant : setters à paramètre `struct` de 9+ octets/champ non primitif, écriture d'un élément struct de tableau ENTIER à champ non primitif, collections concurrentes autres que `ConcurrentDictionary`.

**Validation terrain restante (l'outil existe et est testé sur `KillEngineTestTarget.exe` — ce n'est pas un manque d'outil) :** voir cases `[ ]` de la section "Chantiers ouverts" juste en dessous (Authorized Third-Party Smoke Pass, PHASE 14/17 sur application tierce moderne).

**Décision produit en attente (pas un manque — ton choix) :**
- PHASE 120-B/C/D : Assistant au-delà du mode lecture seule — prérequis arsenal jugés satisfaits par les 4 agents, mais démarrage exclusivement sur feu vert explicite (règle non négociable, voir section PHASE 120-B/C/D plus bas).

**Conclusion :** la suite logique n'est plus "ajouter des outils" mais (a) finir la validation terrain ci-dessous, puis (b) selon ton choix, ouvrir PHASE 120-B/C/D.

## Règle d'utilisation

- Ajouter les nouvelles phases ici, en bas de fichier, avec quoi/pourquoi/comment vérifié.
- Ne pas remettre tout l'historique dans ce fichier : s'il faut un détail ancien, pointer vers `docs/PHASE_TRACKER_HISTORY.md`.
- Ne jamais supprimer d'historique sans transfert explicite.
- Pour les gros détails d'une phase future, préférer un document dédié dans `docs/` puis garder ici un résumé avec lien.

## Chantiers ouverts / validations restantes

- [x] PHASE 13 - Robustesse et validations restantes (close le 28/08/2026, PHASE 200)
  - [x] Passe de régression manuelle V1 sur KillEngineTestTarget validée en direct le 28/08/2026 (PHASE 200) : les 10 étapes du "KillEngineTestTarget Manual Pass" (`docs/V1_REGRESSION_CHECKLIST.md`) rejouées sur `KillEngineTestTarget.exe` (scan exact/narrowing/write/rollback, unknown capture, profil save/reload/activate/write/rollback, journal utilisateur). A révélé et corrigé un vrai manque : l'activation de cible de profil (`activateProfileTarget`) n'écrivait jamais dans le journal utilisateur (`store.addActionLog` non exporté par le store, jamais appelé par `ProfileView.vue::activateTarget`) — corrigé, reconstruit, revérifié en direct via l'UI réelle (pilotée via CDP) : nouvelle entrée `kind:"profile"` confirmée dans le journal.
  - [x] Sections Assistant + Displayed vs Source Classifier closes le 27/08/2026.
  - [x] Reliability Pass étape 2 validée en direct le 27/08/2026 : pointer chain survit à un redémarrage complet sur `KillEngineTestTarget.exe`.
  - [x] Reliability Pass étape 1 validée en direct le 28/08/2026 (PHASE 185) : Freeze BP tient sous charge réelle (49 hits/49 rewrites) sur le tutoriel Cheat Engine (`Tutorial-x86_64.exe`), application tierce contrôlée choisie après échec sur Vampire Survivors (bruit temps réel trop chaotique pour le diff scan) et exclusion volontaire de Solitaire (compte propriétaire réel).
  - [x] Reliability Pass étapes 3/4 validées en direct le 28/08/2026 (PHASE 186) sur `KillEngineTestTarget.exe` : patch AOB (NOP sur l'écriture `g_health`) survit à un redémarrage complet via profil (`saveProfileCodePatch`/`applyProfileCodePatch`, re-résolution AOB, comportement confirmé sous 3 clics réels) ; chaîne de fallback pire cas confirmée sans abandon (`startAutoResolve` enchaîne automatiquement scan exact vide → scan chiffré XOR) et le maillon Unknown capture résout seul `g_hidden_score` (valeur invisible dans l'UI, jamais affichée), confirmé par relecture directe après plusieurs clics.
  - [x] Authorized Third-Party Smoke Pass validée en direct le 28/08/2026 (PHASE 199) sur le tutoriel Cheat Engine (`Tutorial-x86_64.exe`) : scan exact Int32 sur Health (100 → convergence à 1 candidat après clic réel + scan `decreased` puis `exact`), écriture limitée au seul candidat convergé, rollback confirmé restaure la valeur pré-écriture (pas la valeur courante du jeu), freeze polling confirmé tenir au repos par lecture mémoire directe (flicker sous clics rapides attendu et documenté, pas un bug), désactivation du freeze confirmée par reprise de la décroissance libre.

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
- [x] PHASE 185 (28/08/2026, Claude) - Reliability Pass étape 1 (Freeze BP sous charge réelle) validée + bug `setFreezeValue`/breakpoint corrigé, détail dans `docs/PHASE_TRACKER_HISTORY.md`.
- [x] PHASE 186 (28/08/2026, Claude) - Reliability Pass étapes 3/4 (AOB persiste au redémarrage, chaîne de fallback pire cas) validées sur `KillEngineTestTarget.exe`, détail dans `docs/PHASE_TRACKER_HISTORY.md`.
- [x] PHASE 187 (28/08/2026) - Audit croisé 4 agents sur la complétude de l'arsenal ; section dédiée ajoutée en haut du tracker, détail dans `docs/PHASE_TRACKER_HISTORY.md`.
- [x] PHASE 189 (28/08/2026, Claude) - CLR : struct-dans-struct-dans-tableau en écriture vérifié + correctif de staleness documentaire (`docs/KILLENGINE_CLR_INSPECTOR_SPEC.md`, `docs/POWER_UP_ROADMAP.md`), détail dans `docs/PHASE_TRACKER_HISTORY.md`.
- [x] PHASE 190 (28/08/2026, Codex) - Confort profils : export/import pointer map + persistance native des dépendances Trainer, détail dans `docs/PHASE_TRACKER_HISTORY.md`.
- [x] PHASE 192 (28/08/2026, Claude) - CLR : écriture d'un élément struct de tableau ENTIER avec champ non primitif imbriqué, détail dans `docs/PHASE_TRACKER_HISTORY.md`.
- [x] PHASE 193 (28/08/2026, Codex) - Pont Ghidra export/import + édition des dépendances Trainer, détail dans `docs/PHASE_TRACKER_HISTORY.md`.
- [x] PHASE 194 (28/08/2026, Claude) - UX : terme "Hooking" ajouté au Lexique (FR + EN), détail dans `docs/PHASE_TRACKER_HISTORY.md`.
- [x] PHASE 195 (28/08/2026, Codex) - Documentation utilisateur : pont Ghidra, pointer map, dépendances Trainer, détail dans `docs/PHASE_TRACKER_HISTORY.md`.
- [x] PHASE 196 (28/08/2026, Claude) - UX : `InfoDot` dédiés pour Inline hook et Interception MinHook dans Expert, détail dans `docs/PHASE_TRACKER_HISTORY.md`.
- [x] PHASE 197 (28/08/2026, Codex) - Documentation technique : code map profils/Ghidra/Trainer, détail dans `docs/PHASE_TRACKER_HISTORY.md`.
- [x] PHASE 199 (28/08/2026, Claude) - Authorized Third-Party Smoke Pass validée en direct sur le tutoriel Cheat Engine (scan/refine/write/rollback/freeze), détail dans `docs/PHASE_TRACKER_HISTORY.md`.
- [x] PHASE 200 (28/08/2026, Claude) - PHASE 13 close : passe de régression manuelle V1 complète sur `KillEngineTestTarget.exe` + bug réel trouvé et corrigé (activation de profil non loguée dans le journal utilisateur), détail dans `docs/PHASE_TRACKER_HISTORY.md`.

## Journal actif

- [x] PHASE 198 (28/08/2026, Codex) - Documentation : migration des tâches accomplies vers l'historique
  - [x] **Quoi** : transfert des entrées détaillées PHASE 184-197 depuis `docs/PHASE_TRACKER.md` vers `docs/PHASE_TRACKER_HISTORY.md`, sous la section "Transfert PHASE 198 - Journal actif PHASE 184-197". Le tracker actif garde seulement l'état courant, les chantiers ouverts, les résumés courts, et cette entrée de migration.
  - [x] **Pourquoi** : demande propriétaire explicite de refaire une migration après les livraisons PHASE 190-197, pour conserver un tracker actif lisible sans perdre le détail de validation multi-agents.
  - [x] **Comment vérifié** : `rg` vérifie que les phases transférées restent présentes dans l'historique ; relecture du tracker actif pour confirmer que les résumés pointent vers `docs/PHASE_TRACKER_HISTORY.md`. Build non requis, documentation uniquement.
