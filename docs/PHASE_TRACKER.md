> **ATTENTION - Tracker actif allégé (PHASE 184, 28/08/2026)**
> L'historique complet détaillé des phases validées a été transféré dans `docs/PHASE_TRACKER_HISTORY.md` pour ne rien perdre tout en gardant ce fichier lisible. Les agents doivent toujours lire `AGENTS.md` puis ce tracker actif avant d'agir ; consulter l'historique quand une ancienne phase, une décision passée ou un détail de validation est nécessaire.

# KillEngine Phase Tracker

Source of truth produit : `KILLENGINE_PROJECT_SPEC.md`
Historique détaillé : `docs/PHASE_TRACKER_HISTORY.md`
Roadmap power-up : `docs/POWER_UP_ROADMAP.md`

## État courant

- Phase 11 prototype : complète et revalidée via Assistant/profils.
- Phase 12 / polishing V1 : complète.
- Phase 13 : baseline améliorations et passe de régression V1 clôturées le 28/08/2026 (PHASE 200).
- PHASE 120-A : ouverte puis livrée en deux parties le 28/08/2026 (`docs/INVESTIGATION_PLAYBOOK.md` + câblage backend lecture seule). PHASE 120-B/C/D restent hors scope et nécessitent un nouvel accord explicite du propriétaire.
- Prochain focus récurrent : validations restantes PHASE 14 (pointer chains sur application moderne tierce autorisée) et PHASE 17 (debugger expérimental) avant généralisation/release candidate.

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
- ~~Quelques extensions CLR pointues (struct-dans-struct-dans-tableau en écriture, setters à paramètre `struct`, `ConcurrentDictionary`)~~ **Correction (PHASE 189/192/202) : quasi toutes closes.** Ce point était basé sur un `docs/POWER_UP_ROADMAP.md` déjà périmé au moment de l'audit PHASE 187 — `ConcurrentDictionary`, l'écriture struct-dans-struct-dans-tableau et l'écriture d'un élément struct de tableau ENTIER avec struct imbriqué sont livrées ; `ConcurrentStack<T>` est couvert depuis PHASE 202. Reste réellement non couvert et non bloquant : setters à paramètre `struct` de 9+ octets/champ non primitif, collections concurrentes segmentées/work-stealing (`ConcurrentQueue`, `ConcurrentBag`, etc.).

**Validation terrain restante (l'outil existe et est testé sur `KillEngineTestTarget.exe` — ce n'est pas un manque d'outil) :** voir cases `[ ]` de la section "Chantiers ouverts" juste en dessous (PHASE 14/17 sur application tierce moderne).

**Décision produit en attente (pas un manque — ton choix) :**
- PHASE 120-B/C/D : Assistant au-delà du mode lecture seule — prérequis arsenal jugés satisfaits par les 4 agents, mais démarrage exclusivement sur feu vert explicite (règle non négociable, voir section PHASE 120-B/C/D plus bas).

**Conclusion :** la suite logique n'est plus "ajouter des outils" mais (a) finir la validation terrain ci-dessous, puis (b) selon ton choix, ouvrir PHASE 120-B/C/D.

## Règle d'utilisation

- Ajouter les nouvelles phases ici, en bas de fichier, avec quoi/pourquoi/comment vérifié.
- Ne pas remettre tout l'historique dans ce fichier : s'il faut un détail ancien, pointer vers `docs/PHASE_TRACKER_HISTORY.md`.
- Ne jamais supprimer d'historique sans transfert explicite.
- Pour les gros détails d'une phase future, préférer un document dédié dans `docs/` puis garder ici un résumé avec lien.

## Chantiers ouverts / validations restantes

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

Les tâches accomplies PHASE 170-200 ont été migrées vers `docs/PHASE_TRACKER_HISTORY.md` lors de la PHASE 201. Garder ici seulement les chantiers ouverts, les décisions produit encore actives et le journal de la session courante.

## Journal actif

- [x] PHASE 201 (29/08/2026, Codex) - Documentation : migration des tâches accomplies restantes vers l'historique
  - [x] **Quoi** : remplacement de la longue section active "Phases récentes résumées" par un renvoi court vers `docs/PHASE_TRACKER_HISTORY.md`, transfert de l'entrée détaillée PHASE 198 dans l'historique, puis retrait de PHASE 13 des chantiers ouverts après remarque du propriétaire.
  - [x] **Pourquoi** : demande propriétaire explicite de migrer les tâches accomplies du tracker actif vers l'historique pour garder `docs/PHASE_TRACKER.md` lisible et centré sur les chantiers ouverts.
  - [x] **Comment vérifié** : `rg` confirme que les phases accomplies PHASE 13 et PHASE 170-200 restent présentes dans `docs/PHASE_TRACKER_HISTORY.md`; relecture ciblée du tracker actif. Build non requis, documentation Markdown uniquement.

- [x] PHASE 202 (29/08/2026, Codex) - CLR : déballage `ConcurrentStack<T>` en lecture
  - [x] **Quoi** : `tools/clr_inspector/KillEngineClrInspector/ClrSession.cs` reconnaît maintenant `System.Collections.Concurrent.ConcurrentStack<T>` dans `DescribeCollection` et déballe la chaîne `_head` → `_next` en ordre LIFO via `DescribeConcurrentStack`. `tests/clr_targets/KillEngineClrTestTarget/ObjectGraph.cs` ajoute `Inventory.ConcurrentTags` avec un scénario Push/Pop contrôlé. `tools/clr_inspector/KillEngineClrInspector.Tests/EndToEndTests.cs` ajoute le test `ReadObject_UnpacksConcurrentStackInLifoOrder`. Specs CLR mises à jour.
  - [x] **Pourquoi** : feu vert propriétaire pour reprendre les conforts non bloquants listés dans le tracker ; choix de `ConcurrentStack<T>` parce que son layout `_head`/`_next` est lisible passivement avec un risque beaucoup plus faible que les structures segmentées/work-stealing (`ConcurrentQueue`, `ConcurrentBag`).
  - [x] **Comment vérifié** : première tentative `.\scripts\build-clr-inspector.ps1 -Test` bloquée par deux helpers `KillEngineClrInspector.exe` existants qui verrouillaient le binaire ; processus identifiés puis fermés. Relance ensuite réussie : build helper + cible CLR OK, tests xUnit **37/37 verts**. Build C++ `.\scripts\build.ps1` OK ; unitaires C++ `.\build\bin\killengine_unit_tests.exe` **256/256 verts**.
