> **ATTENTION - Priorité Des Ordres Propriétaire**
> Un ordre prioritaire explicite du propriétaire du projet prime sur les consignes temporaires de session des agents IA.

# KillEngine — Playbook d'enquête (PHASE 120-A)

Rédigé le 28/08/2026 dans le cadre de PHASE 120-A (mode réflexion en lecture seule de l'Assistant), suite à un échange stratégique Claude ↔ Codex synthétisé et approuvé par le propriétaire le même jour.

## Ce que ce document EST

Une matrice de référence **déclarative** — symptôme rapporté par l'utilisateur → hypothèses probables → outil conseillé → prérequis → risque → prochaine action humaine recommandée → fallback si ça ne marche pas. L'Assistant **cite et explique** cette matrice dans ses réponses (PHASE 120-A), il ne l'utilise jamais pour décider ou exécuter à sa place.

## Ce que ce document N'EST PAS

- **Pas un troisième moteur de décision.** `AIEngine::processQuery` (`ai/ai_engine.cpp`) et `classifySmartSearchIntent`/`startSmartSearch` (`apps/desktop/application_controller.cpp`) restent les deux seules couches qui décident quoi exécuter (PHASE 177/178 a déjà fermé le risque de les voir diverger). Ce playbook ne route rien, n'exécute rien, ne contourne aucun des deux — il documente le raisonnement qu'un utilisateur expérimenté suivrait, pour que l'Assistant puisse l'expliquer à un débutant.
- **Pas un duplicata** de `docs/KILLENGINE_ASSISTANT_TOOLS_MAP.md` (l'inventaire exhaustif des 28 outils, risque/confirmation/dispatch, déjà préparé explicitement pour PHASE 120) ni de `docs/KILLENGINE_TOOLS_AND_CAPABILITIES.md` (inventaire produit par vue). Ce document part de ces deux-là et les organise **par symptôme utilisateur** plutôt que par outil ou par vue — c'est l'angle qui manquait.
- **Pas un chemin d'automatisation.** Ce playbook ne doit jamais être exposé via le pipe d'automatisation ou un script Lua comme un outil qui "enquête tout seul" — le pipe contourne volontairement `RiskGate`/`requiresConfirmation` (voir `AGENTS.md`, section "Connecteur d'automatisation locale"), et une enquête qui s'appuierait dessus perdrait toute garantie de confirmation humaine. PHASE 120-A reste strictement scopée au chemin chat/UI (`startSmartSearch`).

## Hors scope explicite de PHASE 120-A

- Aucun enchaînement automatique de plusieurs étapes du playbook sans confirmation à chaque étape à risque.
- Aucune orchestration automatisée du cluster encore couplé Trace UI string / AOB / Write dans `ExpertView.vue` (jamais complètement découplé, voir PHASE 18 P5 V1-V5 dans `docs/PHASE_TRACKER.md`) — le playbook peut en parler et rediriger vers l'UI Expert, il ne doit jamais l'orchestrer à la place de l'utilisateur.
- Aucun patch/write/freeze/debug déclenché automatiquement — chaque ligne "Risque" ci-dessous rappelle explicitement si l'action nécessite une confirmation `RiskGate` humaine (elle nécessite, presque toujours).
- PHASE 120-B/C/D (recommandations avec boutons d'action confirmés, enchaînement semi-automatique read-only, escalade complète) restent hors scope, backlog séparé, chacune nécessitant un nouvel accord explicite du propriétaire.

## Matrice symptôme → hypothèses → outil → prérequis → risque → prochaine action → fallback

### 1. Valeur numérique visible à l'écran, simple, pas encore trouvée

- **Hypothèses** : la valeur est stockée telle quelle en mémoire (Int32/Float/etc.), pas de couche d'obfuscation ni de recalcul d'affichage.
- **Outil conseillé** : `exact_scan` (safe, aucune confirmation) sur la valeur actuellement affichée ; `next_scan` pour réduire dès que la valeur change à l'écran.
- **Prérequis** : processus attaché, valeur actuellement visible à l'écran.
- **Risque** : aucun — scan pur lecture seule ; seule une écriture ultérieure (`write_value`/`freeze_value`) nécessite une confirmation `RiskGate`.
- **Prochaine action humaine recommandée** : donner la valeur affichée à l'Assistant ; si elle change de façon prévisible (augmente/diminue/inconnue), utiliser un scan guidé (`GuidedScan`) ou `unknown_capture`/`unknown_compare` plutôt qu'un scan exact répété.
- **Fallback** : si le scan exact ne converge jamais vers un petit nombre de candidats, passer au symptôme 2 (valeur introuvable) plutôt que d'insister sur des scans exacts répétés — signe probable d'un stockage non trivial (texte, obfuscation, type inhabituel).

### 2. Valeur affichée introuvable par un scan numérique classique

- **Hypothèses** : la valeur est rendue à partir d'une string UI plutôt que d'un binaire pur, ou légèrement obfusquée (XOR, offset multiplicatif).
- **Outil conseillé** : `trace_ui_string` (cherche la valeur en ASCII/UTF-16) puis `analyze_ui_sources` (cherche les formes numériques autour des strings suivies) ; `encrypted_scan` si une transformation simple (XOR contre une clé) est suspectée.
- **Prérequis** : la valeur doit être visible à l'écran sous forme de texte lisible.
- **Risque** : aucun — les deux outils sont lecture seule (`analyze_ui_sources` a été délibérément reclassé sans confirmation, PHASE 140, car il ne fait qu'analyser).
- **Prochaine action humaine recommandée** : confirmer la valeur affichée exacte (chaîne de caractères), puis suivre son évolution pour que `analyze_ui_sources` retrouve les sources numériques correspondantes.
- **Fallback** : si aucune source numérique fiable n'émerge, escalader vers le symptôme 6 (« qui écrit cette valeur ») — mais avertir explicitement que `find_what_writes` attache un debugger et nécessite une confirmation.

### 3. Une adresse fonctionne mais ne survit pas à un redémarrage du jeu

- **Hypothèses** : adresse absolue invalidée par l'ASLR (redémarrage = nouvelle base mémoire), ou objet du tas réalloué à une adresse différente à chaque partie.
- **Outil conseillé** : `generate_aob` si l'adresse cible est du **code** (une instruction stable à repérer) ; pour une **donnée** (variable de jeu), utiliser la chaîne de pointeurs Trainer (`scanPointerChains`, PHASE 162 — hors de la liste des 28 outils Assistant, disponible via Trainer/Expert) plutôt qu'une AOB sur de la donnée.
- **Prérequis** : l'adresse doit déjà être validée comme correcte dans la session en cours.
- **Risque** : aucun pour la génération/le scan (lecture seule) ; la transformation en feature Trainer avec le bon `locatorKind` ne fait que sauvegarder une configuration, aucune écriture supplémentaire.
- **Prochaine action humaine recommandée** : transformer la trouvaille en feature Trainer avec un locator AOB (code) ou pointer chain (donnée) plutôt qu'une adresse absolue brute.
- **Fallback** : si ni signature AOB unique ni chaîne de pointeurs stable n'est trouvée, rester en `locatorKind: absolute` mais **le dire explicitement** à l'utilisateur (la feature ne survivra probablement pas à un redémarrage) — jamais de fausse promesse de stabilité (voir PHASE 163).

### 4. Le freeze "clignote" / ne tient pas

- **Hypothèses** : le jeu réécrit la valeur plus vite que l'intervalle de polling du freeze, **ou** l'adresse ciblée est en réalité un champ affiché recalculé à chaque tick (pas la vraie source de la donnée).
- **Outil conseillé** : `analyze_field_stability` **d'abord** (lecture seule, jamais d'écriture, distingue « source réelle » vs « affichage dérivé ») avant d'escalader vers un freeze en mode breakpoint matériel (BP) plutôt que polling.
- **Prérequis** : une adresse déjà identifiée comme candidate au freeze.
- **Risque** : `analyze_field_stability` = aucun risque (confirmation délibérément désactivée, PHASE 130/131, car jamais d'écriture). Le freeze BP attache un debugger et nécessite une confirmation `RiskGate` — plus fiable que le polling sous forte cadence (~80% de tenue mesurée, voir `AGENTS.md`) mais reste une opération plus invasive.
- **Prochaine action humaine recommandée** : analyser la stabilité du champ avant de basculer en BP à l'aveugle — ne pas escalader directement sur un simple signalement "ça clignote".
- **Fallback** : si le champ est jugé « affichage dérivé » (recalculé), chercher la vraie source événementielle via le symptôme 6 plutôt que de freezer un symptôme qui ne tiendra jamais durablement.

### 5. L'utilisateur demande un patch de code (NOP, forcer un saut, etc.)

- **Hypothèses** : l'utilisateur veut modifier une instruction plutôt qu'une simple valeur de donnée.
- **Outil conseillé** : `generate_aob` → `suggest_patch` → `disassemble_backward` si besoin de contexte — les trois sont lecture seule (suggèrent sans jamais appliquer). L'application réelle (`applyCodePatch`) reste **hors surface Assistant** par conception : toujours redirigée vers l'UI Expert/Trainer, jamais auto-exécutée depuis le chat.
- **Prérequis** : une adresse de code valide (RIP capturé via un hit `find_what_writes`, ou déjà connue).
- **Risque** : élevé (`risk=patch`) — modifie du code exécutable. Signature ambiguë (plusieurs correspondances) = danger réel de patcher au mauvais endroit. Sur certaines machines, un EDR peut bloquer l'écriture (`ERROR_ACCESS_DENIED`, voir `docs/STRATEGY_ROOM.md` — connu et documenté, pas un bug KillEngine).
- **Prochaine action humaine recommandée** : toujours vérifier l'unicité de la signature avant application réelle (dans l'UI, pas depuis le chat) ; l'Assistant doit dire explicitement que l'application se fait dans Expert/Trainer, jamais promettre de l'appliquer lui-même.
- **Fallback** : si la signature n'est pas unique ou si l'EDR bloque l'écriture, ne pas insister — proposer un freeze/write classique comme alternative moins invasive.

### 6. « Qui écrit cette valeur ? » — comprendre l'origine d'une écriture

- **Hypothèses** : plusieurs sites de code peuvent potentiellement écrire la même adresse ; il faut identifier lequel est la vraie source de gameplay.
- **Outil conseillé** : `find_what_writes` — **jamais exécuté de façon autonome** (PHASE 140) : redirige systématiquement vers le bouton « Écrit par » de l'UI Expert, qui nécessite une confirmation et une action utilisateur pendant la fenêtre de capture.
- **Prérequis** : une adresse **stable**, pas une adresse « chaude »/générique (allocateur réutilisé à haute fréquence). Un cas réel documenté cette session a fait planter le processus cible en armant un breakpoint sur une adresse de ce type (10 écritures en ~15s depuis 10 sites de code différents).
- **Risque** : attache un debugger — peut perturber ou faire planter la cible, en particulier sur une adresse chaude ou un processus sandboxé/fragile (`debug`, confirmation obligatoire).
- **Prochaine action humaine recommandée** : confirmer explicitement avant de lancer, avertir l'utilisateur qu'il doit interagir avec le jeu pendant la fenêtre de capture (pas après).
- **Fallback** : si aucun hit capté, élargir la fenêtre de capture ou revérifier que l'adresse est bien une adresse par-objet stable, pas un slot d'allocateur générique.

### 7. La valeur vit dans un fichier de sauvegarde / le registre UWP, pas en mémoire vive

- **Hypothèses** : le jeu persiste la valeur sur disque (fichier de sauvegarde) ou dans les `LocalSettings` UWP plutôt qu'en RAM accessible directement.
- **Outil conseillé** : `discover_save_files` → `read_save_file_text`/`inspect_local_settings` ; `watch_save_file` si la valeur change en direct et qu'il faut observer la réécriture.
- **Prérequis** : processus UWP ou jeu avec un fichier de sauvegarde identifiable.
- **Risque** : aucun en lecture (`safe`). `patch_file_bytes` (écriture disque) existe mais reste **hors schéma modèle** (PHASE 148, jamais choisi librement par l'IA) — toujours redirigé vers une action UI explicite, jamais exécuté depuis le chat.
- **Prochaine action humaine recommandée** : comparer un snapshot avant/après une action utilisateur pour isoler le champ modifié.
- **Fallback (limite honnête de PHASE 120-A)** : si le fichier est réécrit par une source plus autoritaire au chargement (ex. cache WebView/IndexedDB local, cas réel déjà rencontré — voir [[solitaire_memory_editing_technique]]), un vrai protocole d'enquête (snapshots complets, fermeture/relance contrôlée, réseau bloqué, purge de cache ciblée) est nécessaire. **Ce protocole dépasse le scope lecture-seule de PHASE 120-A** — l'Assistant doit le dire honnêtement plutôt que de suggérer une solution simple qui ne marchera pas.

## Rappel transverse (vaut pour toutes les entrées ci-dessus)

- Le pipe d'automatisation et les scripts Lua **ne respectent jamais `requiresConfirmation`** (bypass volontaire documenté, `AGENTS.md`). Ce playbook et le raisonnement PHASE 120-A qui s'appuie dessus concernent **exclusivement** le chemin chat/UI (`startSmartSearch`) — jamais une automatisation via le pipe.
- Une entrée de ce playbook qui recommande un outil `risk=write`/`debug`/`patch` doit toujours rappeler qu'une confirmation `RiskGate` humaine est requise avant toute exécution réelle — l'Assistant explique et recommande, il n'agit jamais à la place de la confirmation.
- Source de vérité pour risque/confirmation/dispatch à jour : `docs/KILLENGINE_ASSISTANT_TOOLS_MAP.md` (relire avant de faire confiance à ce document si le registre a changé depuis le 28/08/2026).

## Liens

- `docs/KILLENGINE_ASSISTANT_TOOLS_MAP.md` — inventaire exhaustif des 28 outils Assistant (risque/confirmation/dispatch), base factuelle de ce playbook.
- `docs/KILLENGINE_TOOLS_AND_CAPABILITIES.md` — inventaire produit par vue.
- `AGENTS.md` — détail par module, pièges connus.
- `docs/PHASE_TRACKER.md` — PHASE 120 (contexte historique du différé), PHASE 177/178 (unification du triage conversationnel, contrainte structurelle pour ce document), PHASE 120-A (implémentation backend de ce playbook).
