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
- PHASE 14A/14B, PHASE 17 et PHASE 120-A/B/C/D : clôturées et archivées dans `docs/PHASE_TRACKER_HISTORY.md`.
- PHASE 202/203/204 : tâches accomplies transférées dans `docs/PHASE_TRACKER_HISTORY.md`.
- `chat_memory_write`/`chat_memory_freeze`/`rewrite_last_auto_write` (chat-origin) : gatés derrière une confirmation explicite le 29/08/2026, archivé dans `docs/PHASE_TRACKER_HISTORY.md` — mode Auto (UI) volontairement inchangé.
- Prochain focus récurrent : aucun chantier de validation terrain, phase 120 ou décision produit RiskGate restant dans "Chantiers ouverts". Reste seulement le branchement éventuel de `InvestigationView.vue`.

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

**Validation terrain :** PHASE 14A/14B et PHASE 17 clôturées le 29/08/2026 (archivées dans `docs/PHASE_TRACKER_HISTORY.md`) — plus de case `[ ]` de validation terrain ouverte dans la section "Chantiers ouverts" juste en dessous au moment de cette mise à jour.

**Décision produit en attente (pas un manque — ton choix) :**
- ~~PHASE 120-B/C/D : Assistant au-delà du mode lecture seule — prérequis arsenal jugés satisfaits par les 4 agents, mais démarrage exclusivement sur feu vert explicite.~~ **Clôturée le 29/08/2026** : B, C et D ont chacune reçu l'accord propriétaire requis, sont livrées et archivées dans `docs/PHASE_TRACKER_HISTORY.md`. Restent seulement deux décisions séparées : branchement éventuel de `InvestigationView.vue` et clarification produit sur `chat_memory_write`/`chat_memory_freeze` sans RiskGate.

**Conclusion :** la suite logique n'est plus "ajouter des outils" ni "ouvrir PHASE 120" : la série PHASE 120-A/B/C/D est close. Les seuls points encore visibles ici sont des décisions produit séparées.

## Règle d'utilisation

- Ajouter les nouvelles phases ici, en bas de fichier, avec quoi/pourquoi/comment vérifié.
- Ne pas remettre tout l'historique dans ce fichier : s'il faut un détail ancien, pointer vers `docs/PHASE_TRACKER_HISTORY.md`.
- Ne jamais supprimer d'historique sans transfert explicite.
- Pour les gros détails d'une phase future, préférer un document dédié dans `docs/` puis garder ici un résumé avec lien.

## Chantiers ouverts / validations restantes

- [ ] Décision séparée - Branchement UI `InvestigationView.vue` au système d'enquête avancée
  - [ ] À planifier seulement après feu vert produit explicite ; ce point n'empêche pas de considérer PHASE 120-B/C/D comme close.

- [x] Décision séparée - `chat_memory_write`/`chat_memory_freeze` sans RiskGate **clôturée le 29/08/2026**, détail complet archivé dans `docs/PHASE_TRACKER_HISTORY.md`. Résumé : le propriétaire a tranché — corriger le point d'entrée chat spécifiquement (`writeChatMemoryTargetsFromQuery`/`freezeChatMemoryTargetsFromQuery`/`rewriteLastAutoWriteTargets` quand 100% chat-origin via nouveau tag `AutoWriteTarget::chatOrigin`), sans toucher au mode Auto (UI guidée) qui reste inchangé par design. Version initiale ajoutait un vrai modal `confirmRiskAction` après le clic du bouton chat, retirée à la demande du propriétaire (testé en direct, jugé redondant vu que la carte chat affiche déjà l'avertissement + le libellé exact) — le clic sur le bouton du chat est désormais la seule et unique confirmation. Validé en direct par le propriétaire sur `KillEngineTestTarget.exe` : plus d'écriture silencieuse, exécution confirmée après un seul clic.

## Phases récentes résumées

Les tâches accomplies PHASE 170-204 ont été migrées vers `docs/PHASE_TRACKER_HISTORY.md`. Garder ici seulement les chantiers ouverts, les décisions produit encore actives et le journal de la session courante.

## Journal actif

Aucune phase accomplie détaillée ne reste dans le journal actif après la migration PHASE 204. Ajouter ici seulement le prochain chantier en cours ; archiver les chantiers clos dans `docs/PHASE_TRACKER_HISTORY.md`.
