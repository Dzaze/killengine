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

## Validations restantes

- [ ] Résidu mineur "confort" restant, jamais promu en PHASE ni explicitement classé "décision produit de ne pas faire" (audit produit/QA/doc, PHASE 236, 30/08/2026 — demande explicite du propriétaire de vérifier qu'aucun chantier produit/QA/doc ne restait non tracké après la clôture du refactor) :
  - `docs/POWER_UP_ROADMAP.md` section K (Lua) : le décodeur JSON maison de `ke.decode_json` ne gère pas les échappements `\uXXXX` — jamais rencontré en pratique, jamais corrigé.
  - Ne bloque aucune phase produit : à prendre seulement si un futur chantier touche déjà cette zone, même règle que le chantier refactor ci-dessus.

## Journal actif

- [x] PHASE 238 - Référence exhaustive des réponses `Q_INVOKABLE` (Claude, 30/08/2026). Chantier repris dès le déblocage explicite de PHASE 233 (refactor entièrement clos).
  - Diagnostic : `docs/AUTOMATION_API.md` documentait la signature des paramètres et la forme de réponse d'un petit cluster (profils, PHASE 233), mais rien pour le reste des ~200 méthodes `Q_INVOKABLE` — un agent devait lire le `.cpp` du manager concerné à chaque fois pour connaître la forme exacte du `QVariantMap` retourné.
  - Correctif : nouveau `docs/AUTOMATION_API_REFERENCE.md` (extraction déléguée à un sous-agent pour ne pas saturer le contexte principal sur un travail mécanique de lecture répétée), groupé par domaine fonctionnel (scan, write/freeze, trainer, profils-suite, CLR inspector, patch/AOB, debug/breakpoints, settings/diagnostics, automation pipe, workspace/pointer chains, Lua/AutoAsm, chat/smart-search/auto-resolve, injection/hooking, save-file/UWP, kernel driver, réseau/speedhack). 199 méthodes documentées directement depuis le code réel (`result["clé"] = ...` lu, jamais deviné), 4 renvoyées vers `docs/AUTOMATION_API.md` (cluster profils déjà documenté, pas dupliqué), 2 marquées "non résolu" honnêtement (payload CLR opaque défini par le helper .NET externe, branches internes du moteur IA local) plutôt que devinées. `docs/AUTOMATION_API.md` mis à jour pour pointer vers ce nouveau doc et clarifier que la convention `Select-String` reste la voie de vérité pour toute méthode modifiée après la génération.
  - Piège rencontré et documenté explicitement plutôt que masqué : au moment de l'extraction, le chantier Structure Analyzer (PHASE 239 ci-dessous) était en cours côté Codex, non commité — `inferStructureInstanceDelta` a donc été capturé en plein développement, marqué avec un avertissement explicite dans le doc plutôt que présenté comme stable (aucun fichier de ce chantier en cours n'a été touché, uniquement lu). Une fois PHASE 239 commitée (`75183a3`), re-vérifié contre le code final : la forme capturée en cours de route avait effectivement changé (`candidateCount` manquant), corrigé dans le doc — preuve concrète a posteriori que la mise en garde "instantané figé" du document n'était pas théorique.
  - Vérification : scan mojibake standard propre sur les 2 fichiers touchés/créés (`docs/AUTOMATION_API.md`, `docs/AUTOMATION_API_REFERENCE.md`), pas de BOM. Pas de build/tests nécessaires (aucun code touché). Relecture manuelle de plusieurs sections (scan, réseau/speedhack, méthodes scalaires) pour vérifier la cohérence avant publication.
  - Roadmap : `docs/PHASE_TRACKER.md` "Validations restantes" — item automation-api exhaustif retiré (fait).
- [x] PHASE 239 - Structure Analyzer : delta automatique entre instances (Codex, 30/08/2026)
  - Diagnostic : le résidu `docs/POWER_UP_ROADMAP.md` section D point 3 restait vrai malgré la clôture refactor : après deux dissections HP joueur 1 / joueur 2, l'utilisateur devait encore calculer à la main l'espacement entre bases et l'offset du champ homologue pour inspecter les autres instances probables.
  - Correctif : ajout de `killcore::inferStructureInstanceDelta` dans `core/scanner/structure_analyzer.{h,cpp}` avec offsets A/B, delta de layout, stride d'instance et candidats extrapolés bornés ; exposition via `ApplicationController::inferStructureInstanceDelta` + `ui/src/services/backend.ts` (interface et mock) ; ajout dans `ExpertView.vue` d'un bouton `Delta instances` entre Capture A/B et d'un bouton `Struct` pour relancer une dissection sur une instance probable.
  - Vérification : `.\scripts\build.ps1` OK ; `.\build\bin\killengine_unit_tests.exe` OK (261/261, nouveaux tests `StructureAnalyzer.InfersInstanceDeltaFromMatchingFields` et `StructureAnalyzer.ReportsMismatchedFieldOffsetsWithoutPredictions`) ; `cd ui && npm run type-check` OK ; `cd ui && npm run build` OK ; pipe automation OK sur `inferStructureInstanceDelta("1000","1018","1200","1218",{beforeCount:1,afterCount:2})` -> `instanceDelta=512`, `fieldOffsetA=24`, `candidateCount=4`.
