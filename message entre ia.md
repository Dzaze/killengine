═ KillEngine — Session du 09/08/2026 ═══════════════════════════════

DEPUIS LE DÉBUT DE LA CONVERSATION, MODIFICATIONS EFFECTUÉES :

────────────────────────────────────────────
1. PHASE 10 — FULL GUIDED WORKFLOW UX
────────────────────────────────────────────

▸ ui/src/stores/app.ts
  - Ajout interface ChatMessage (role, text, workflowStatus, candidateCount, suggestions, autoWriteResults...)
  - Nouvel état : messages[], workflowStatus, targetValueGuided, candidateHistory, isSearching, showExpertPanel
  - Nouvelles actions : pushMessage(), doGuidedChange(), tellNewValue(), resetWorkflow()
  - doSearch() réécrite pour alimenter le chat + synchroniser les panneaux experts
  - Nouvelle action rollbackLastWriteBatch()

▸ ui/src/views/AssistantView.vue
  - Refonte complète : interface de chat guidée (messages, avatars, timestamps)
  - Empty state avec exemples cliquables
  - Badge de workflow dynamique (couleurs selon statut)
  - Cartes d'auto-write avec adresses + statut vérifié
  - Bouton "Rollback toutes les écritures" dans le chat
  - Quick action "J'ai changé" contextuelle
  - Thinking dots pendant la recherche
  - Panneau Expert repliable (contrôles manuels conservés)

▸ ui/src/services/backend.ts
  - Ajout rollbackLastWriteBatch() dans l'interface BackendController + mock

▸ apps/desktop/application_controller.h
  - Nouvelle méthode Q_INVOKABLE rollbackLastWriteBatch()
  - Nouveau struct WriteRecord { address, previousValue }
  - Nouveaux membres : m_writeHistory, m_lastBatchStartIndex

▸ apps/desktop/application_controller.cpp
  - Implémentation rollbackLastWriteBatch() : restaure toutes les écritures du batch
  - writeMemoryValue() enregistre maintenant chaque écriture dans m_writeHistory
  - Auto-write results enrichis avec address, value, type pour l'affichage UI
  - detachProcess() nettoie m_writeHistory
  - Message rollbackNote mis à jour

────────────────────────────────────────────
2. VALIDATION DES PHASES PRÉCÉDENTES
────────────────────────────────────────────

▸ tests/memory_targets/test_target_main.cpp — NOUVEAU
  - KillEngineTestTarget.exe : application Qt avec variables connues
  - Variables : health(Int32), money(Int32), stamina(Float32), position(Float64)
  - hidden_score pour Phase 7 (Unknown Initial Value)
  - Player* (heap) pour tests adresses dynamiques
  - Boutons : Spend/Gain, Damage/Heal, Drain Stamina, Relocate, Unknown Inc/Dec/Rand, Reallocate Player
  - Timer de bruit mémoire (faux positifs)
  - Débloque les validations Phases 3, 4, 6, 7, 8

▸ tests/CMakeLists.txt
  - Ajout target KillEngineTestTarget (AUTOMOC, WIN32_EXECUTABLE)

▸ tests/unit/test_candidate_store.cpp
  - Nouveau test HandlesLargeCandidateSet : 100 000 candidats
  - Valide pagination + filtrage sur large set (Phase 5)

────────────────────────────────────────────
3. DOCUMENTATION
────────────────────────────────────────────

▸ docs/PHASE_TRACKER.md
  - Phase 10 : [x] complète (full guided workflow UX coché)
  - Phase 3 : [x] validation KillEngineTestTarget
  - Phase 4 : [x] validation KillEngineTestTarget
  - Phase 5 : [x] validation large candidate set
  - Phase 6 : [x] validation KillEngineTestTarget
  - Phase 7 : [x] validation KillEngineTestTarget (LZ4/mapped restent)
  - Phase 8 : [x] validation KillEngineTestTarget

▸ KILLENGINE_PROJECT_SPEC.md
  - Baseline mise à jour : 13/13 tests, KillEngineTestTarget.exe build OK
  - Limites connues mises à jour (rollback batch, TestTarget créé)

────────────────────────────────────────────
RÉSULTAT BUILD
────────────────────────────────────────────
  UI build OK
  C++ build OK
  Tests unitaires : 13/13 OK
  KillEngineTestTarget.exe : build OK

RESTE À FAIRE :
  - Phase 7 : LZ4 compression + mapped storage (features à coder)
  - Phase 9 : llama.cpp + Qwen GGUF (intégration lourde)
  - Phase 11 : Profils
  - Phase 12 : Polissage V1

────────────────────────────────────────────
MISE À JOUR — 09/08/2026, reprise Phase 12
────────────────────────────────────────────

Travaux récents validés et poussés :

▸ Assistant / conversation mémoire
  - Les adresses données directement dans le chat peuvent devenir des cibles actives.
  - Les dernières adresses auto-écrites restent réutilisables dans les demandes suivantes.
  - Une demande explicite de nouvelle recherche remet de côté les cibles actives de conversation/profil.

▸ Profils
  - Un profil peut contenir plusieurs cibles.
  - Les cibles résolues peuvent être activées pour l'Assistant.

▸ Mode Expert / Paramètres / diagnostics
  - Mode Expert V1 avec filtres d'adresse, alignement et protections mémoire.
  - Paramètres persistants : langue, type par défaut, limites de scan, fast scan, debug Smart Search, placeholders IA.
  - Lecture des logs et export diagnostic zip.
  - Crash reports locaux inclus dans l'export diagnostic.

▸ Packaging
  - Script zip portable et template Inno Setup ajoutés.

▸ Scans asynchrones
  - Scan exact en worker thread via startExactScanAsync + signal scanFinished.
  - Next scan en worker thread via nextScanAsync + signal scanFinished.
  - Capture unknown en worker thread via captureUnknownSnapshotAsync + signal scanFinished.
  - Comparaison unknown en worker thread via unknownNextScanAsync + signal scanFinished.
  - SnapshotStore possède une sémantique de move explicite pour transférer proprement le fichier temporaire memory-mapped du worker vers le contrôleur UI.
  - Annulation du scan actif via cancelActiveScan.
  - Une annulation ne remplace pas le CandidateStore avec des résultats partiels.
  - Le Mode Expert affiche un bouton Annuler pendant un scan actif.
  - detachProcess() ne nettoie plus l'état mémoire pendant qu'un scan est en cours ; il demande l'annulation et attend que le worker termine.

Dernières validations connues :
  - npm run build : OK
  - .\scripts\build.ps1 : OK
  - ctest --test-dir build --output-on-failure : 24/24 OK

RESTE À FAIRE ACTUEL :
  - Phase 12 checklist V1 : complète.
  - Phase 13 ouverte dans docs/PHASE_TRACKER.md comme baseline d'améliorations :
    précision de recherche, faux positifs, Assistant plus naturel, profils plus utiles, UX/efficacité, robustesse/tests.
  - Prochaine étape : traiter les points Phase 13 un par un, puis passe de régression manuelle V1 / release candidate.
  - Tests d'intégration automatisés à écrire si on veut couvrir les scénarios complets hors validation manuelle.
