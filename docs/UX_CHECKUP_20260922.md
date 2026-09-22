# Checkup UX et code via pipe — 22/09/2026

Demande propriétaire : se connecter à KillEngine par pipe et examiner largement l'expérience utilisateur, l'ordre des pages, les options et le code, puis proposer des améliorations. Audit réalisé par Codex ; aucun correctif applicatif livré dans cette session.

## Périmètre réellement vérifié

- Aucun KillEngine initialement lancé ; premier ping expiré après 2 s. Lancement de `build/bin/KillEngine.exe` avec le pipe et CDP local sur `127.0.0.1:9432`.
- Pipe `KillEngineAutomationPipe` confirmé par `ping`, `getVersion`, `getAutomationPipeStatus`. Version annoncée : `0.1.0`.
- Binaire du 18/09/2026 18:24:29 ; SHA256 `24E30F99D4527F5C6F1B47DCC0E9FFA7E344F819704FDA7CD1A5257E9DFD1A2F`.
- UI réellement chargée : `ui/dist/assets/index-BIu6op3p.js`, du 19/09/2026 12:41:10 ; SHA256 `752CCD1F3520DA3ED1656C1B32196B47485AB7F75F92D703A520226870E3C7BB`.
- Parcours des 18 destinations du menu via leurs boutons réels ; lecture DOM et captures PNG. Vue CSS 1282 × 802 ; captures PNG 1602 × 1002.
- Lecture des statuts IA, Modules, CLR, Lua, WebView2, Heatmap, Timeline, recherche et stockage temporaire par pipe. Aucun appel au fournisseur IA externe.
- Le préchauffage proposait une estimation de 3 min 37 s ; clic sur « Continue without local AI » pour poursuivre l'audit des outils déterministes. Ce choix de session ne constitue pas une mesure du temps de préchauffage réel.
- Cible contrôlée `KillEngineTestTarget.exe`, PID 19416, attachée via pipe. Tests de lecture/collecte uniquement ; aucune écriture, aucun freeze, patch ou changement système. Adresse de test health issue du marqueur : `0x7ff778cf10e0`.
- Nettoyage : watch ajouté retiré, collecte arrêtée, adresses Timeline supprimées, cible détachée, deux processus lancés pour l'audit arrêtés. La fermeture normale de l'application cachée ne répondait pas à `CloseMainWindow`, donc arrêt de son PID après nettoyage.
- Preuves locales dans `dist/ux-checkup-20260922/` : réponses pipe, descriptions DOM et captures. Ce dossier est un artefact local, pas un remplacement des références au code ci-dessous.

Les modifications applicatives déjà présentes dans le dépôt appartiennent aux chantiers antérieurs et ont été conservées. Pas de build ni de suite unitaire relancés : travail de diagnostic/documentation, sans modification du code. Le build disponible et le code source actuel sont distingués ; aucun résultat live n'est présenté comme validation d'une recompilation du dépôt.

## Priorité haute — comportements à corriger

### 1. Timeline : état affiché différent du moteur et arrêt à la navigation

**Reproduit.** Ajout de health par le formulaire, durée 1000 ms, puis Start. Après 1600 ms, le pipe retourne `collecting:false`, `totalDataPoints:1`, `watchedAddressCount:1`. L'UI affiche encore « Collecting... » et « Starting... », Start désactivé, Stop disponible. La valeur testée est stable et « Track changes only » actif : un seul point est attendu, ce n'est pas le défaut.

Second essai avec 60000 ms : pipe `collecting:true` avant navigation ; clic Expert ; pipe `collecting:false` immédiatement après. Aucun avertissement d'arrêt.

**Cause corroborée** : `ui/src/views/MemoryTimelineView.vue:512` écoute trois événements DOM sans émetteur trouvé dans le frontend/backend ; les statistiques locales initialisées ligne 219 ne sont pas actualisées. `onUnmounted`, ligne 529, arrête la collecte. Les listeners anonymes ne sont pas retirés. Heatmap a un comportement différent : quitter la vue arrête son polling UI, pas sa collecte (`MemoryHeatmapView.vue:112`).

**Piste** : statut d'opération central, abonnement réel ou polling borné, réconciliation au montage, nettoyage des listeners et arrêt explicite. Ajouter un indicateur global si la collecte doit continuer hors de sa page. Tester fin automatique, arrêt manuel, navigation et collecte lancée via pipe.

### 2. Suggestions de types : variantes incorrectes et multiplication répétée

**Reproduit par saisies/clics réels, sans lancer de scan.** Saisir 50 puis cliquer `Int32 x4096` laisse la valeur 50 et rend le sélecteur vide (`selectedIndex:-1`). Saisir 50 puis cliquer `Int32 x100` donne 5000 ; cliquer une deuxième fois donne 500000.

**Cause** : `ui/src/stores/app.ts:4440` génère cinq facteurs, mais `useInferredType`, ligne 4451, ne traite que x100 et modifie directement la saisie. Boutons : `ui/src/components/expert/ExactScanPanel.vue:54`.

**Piste** : trois données distinctes — valeur affichée, type natif, facteur — et un aperçu « 50 affiché → 204800 stocké, Int32 ×4096 ». Le choix doit être idempotent. Tester tous les facteurs et les clics répétés ; éviter la perte de précision sur les valeurs hors plage.

### 3. Scan Auto : les filtres Expert ne sont pas transmis

**Confirmé par code, pas par mesure live d'un scan.** `ui/src/stores/scanning.ts:211` calcule les filtres, mais la branche Auto ligne 231 appelle `startExactScanMultiType(value,type)` sans options. Les filtres sont transmis dans la branche mono-type ligne 247 seulement. `apps/desktop/scanning_core_manager.cpp:841` reprend les réglages globaux ; ligne 848 lance ce chemin de manière synchrone, sans token d'annulation.

**Impact** : l'utilisateur peut délimiter une plage ou des permissions et croire que le scan Auto les respecte. La durée d'un gros scan et le blocage UI potentiel n'ont pas été mesurés ici.

**Piste** : contrat d'options commun mono/multitype, même progression et annulation. Test sur cible contrôlée comportant une correspondance dans la plage et une hors plage.

## Priorité suivante — confiance dans les états et les erreurs

### 4. Watch live : une erreur de lecture ressemble à une absence de valeur

**Reproduit.** `callVueStoreAction("addAddressToWatch", ["1","Int32"])` crée la ligne. Le pipe confirme `success:false`, zéro octet et `ReadProcessMemory failed at 0x1 (error=299)`. La ligne UI n'affiche que `0x1`, `Int32`, `-`, l'heure et Remove.

**Cause** : l'erreur est conservée dans `app.ts:4517`, mais ignorée par le rendu `WatchLivePanel.vue:45`.

**Piste** : état « illisible », cause accessible, distinction dernière tentative/dernière réussite. Associer le watch à une identité de session ; la remise à zéro à l'attachement (`app.ts:3480`) ne traite pas les watches. Empêcher deux passages de polling simultanés : le timer ligne 4544 n'attend pas la fin des lectures séquentielles ligne 4530. Ces deux derniers risques sont identifiés au code, pas reproduits par changement de cible ou charge de 200 watches.

### 5. Assistant : afficher le mode IA effectivement disponible

**Observé.** Après le choix sans IA locale, Modules affiche correctement « Local AI disabled for this session ». Assistant reste présenté comme un chat qui choisit les outils ; son bandeau ne précise pas ce mode. `AssistantView.vue:80` calcule le statut avec recherche/workflow/attachement, pas `sessionDisabled` ni le fournisseur ; rendu ligne 934.

**Piste** : indicateur permanent « IA locale », « Claude » ou « commandes déterministes », disponibilité réelle et action adaptée. Ne pas proposer de nouveau fournisseur : le périmètre produit existant reste inchangé. UX-PIPE-2 avait déjà corrigé Modules/Paramètres ; ce constat vise Assistant.

### 6. Mutualiser l'exécution asynchrone côté frontend

**Analyse de code uniquement.** Les scans exacts s'abonnent avant le démarrage et tamponnent les événements précoces (`backend.ts:2182`). Les chemins Unknown s'abonnent après la réponse de démarrage (`backend.ts:2353`, `:2401`) et ne retirent pas le handler dans leur branche timeout. La course à l'événement n'a pas été reproduite pendant cet audit.

**Piste** : helper commun pour abonnement anticipé, identifiant de requête, nettoyage garanti, annulation et réconciliation avec le moteur après timeout. Tests ciblés sur réponse très rapide, retard, annulation et changement de cible.

## Organisation proposée — choix produit à distinguer des bugs

### 7. Réduire le coût de navigation entre les 18 pages

**Observation** : menu plat, Modules en deuxième position, Expert en seizième ; le menu défile dans la fenêtre réelle (`ui/src/App.vue:101`).

Proposition de regroupement, en conservant les vues spécialisées existantes :

- Parcours principal : **Processus → Assistant → Investigation → Expert → Profils → Trainer**.
- Inspection : Mémoire, Timeline, Heatmap, Pattern Learning, CLR, WebView2, Réseau.
- Outils avancés : Lua, Speedhack.
- Configuration/aide : Composants et diagnostics, Paramètres, Lexique/Aide.

Un sélecteur de cible persistant près de la navigation réduirait les retours à Processus. Les pages spécialisées pourraient indiquer leurs prérequis et proposer un accès direct à la cible ou au composant manquant.

Renommer la page « Modules » en « Composants et diagnostics » éviterait sa confusion avec les DLL/modules chargés de la page Processus (`ModulesView.vue:448`, `ProcessView.vue:232`).

### 8. Sortir les projets et les notes de Paramètres

**Mesuré** : 73 boutons, hauteur de contenu 7847 px pour une zone visible de 802 px. La page mélange préférences, IA, projets, notes, imports, audits, noyau, diagnostics et journaux.

**Piste** : accès « Projet/session » pour sauvegarde, reprise, notes, import/export et journal ; Paramètres conservé pour les préférences ; diagnostics techniques regroupés. Ajouter une recherche ou des sections navigables. Points d'entrée : `SettingsView.vue:708`, `:746`, `:800`. La pagination est déjà corrigée par UX-PIPE-9, ce n'est pas un nouveau constat de troncature.

### 9. Expert : exploiter les étapes existantes et ajouter des raccourcis vers les outils

**Mesuré** : 112 boutons et 5235 px de contenu après attachement, mode « All ». Les quatre étapes Trouver/Inspecter/Agir/Pérenniser existent déjà ; le défaut « Tout » est intentionnel pour garder l'écriture accessible (`ExpertView.vue:1809`).

**Piste** : conserver cette visibilité, ajouter un sommaire fixe, une recherche « Aller à un outil » et des outils épinglés. Les ancres et `scrollIntoView` existent déjà (`ExpertView.vue:1920`) et sont appelés depuis Assistant : les réutiliser. Aucune recherche ou liste de favoris d'outils identifiée lors de cette passe. Ne pas annoncer comme nouveau le découpage en étapes déjà livré.

### 10. Investigation : présenter la situation avant les formulaires

Le carnet et le formulaire de preuve arrivent avant l'objectif, la prochaine action et même l'état vide (`InvestigationView.vue:378`, `:482`, `:671`, `:728`, `:743`).

**Piste** : bandeau « cible / objectif / preuves disponibles / prochaine action », puis formulaire pertinent et journal. Conserver la séparation écriture confirmée/effet confirmé/solution durable déjà livrée. Cette proposition d'ordre visuel ne rouvre pas le chantier du carnet d'hypothèses.

### 11. Lisibilité et diagnostics secondaires

- Éclaircir les textes informatifs secondaires : `--text-dim:#565f89` sur `#16171f`, avec des descriptions de 11–12 px (`App.vue:306`, `ExpertView.vue:2979`, `PanelIntro.vue:49`). Les captures montrent une hiérarchie visuelle très atténuée ; réserver cette atténuation aux contrôles réellement inactifs.
- Dédupliquer les agents IA logiques : le pipe annonce quatre agents, mais ce sont les deux identifiants assistant/auto_resolver dans deux racines, dépôt et build. `settings_diagnostics_manager.cpp:326` déduplique les chemins, pas les IDs, puis compte les manifestes ligne 441. Afficher les deux rôles et la provenance effectivement retenue ; conserver les copies secondaires dans le diagnostic détaillé.
- Corriger séparément le résumé documentaire : `PHASE_TRACKER.md:20` présente UX-PIPE-7 à 10 ouverts, alors que leurs fiches/tableau lignes 849–852 les donnent clos. Cet audit se base sur les fiches détaillées et ne les rouvre pas.

## Ordre de réalisation conseillé

1. Timeline, variantes numériques et respect des filtres Auto.
2. Erreurs Watch, mode IA effectif et contrat des opérations asynchrones.
3. Navigation, séparation Projet/Paramètres, accès rapide aux outils Expert, ordre d'Investigation.
4. Lisibilité et exactitude des diagnostics secondaires.

Étendre le harnais **existant** `scripts/test-ui-journeys.ps1` aux régressions reproduites plutôt que créer une deuxième infrastructure : fin automatique/navigation Timeline, variantes, watch illisible et IA désactivée. Les quatre parcours déjà présents et la vérification d'annulation ne sont pas absents. Après les futurs correctifs : build complet, tests unitaires pertinents et rejeu du parcours réel concerné.
