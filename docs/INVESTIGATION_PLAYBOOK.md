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
- **Outil conseillé** : **si l'adresse vient d'un hit `find_what_writes`, `disassemble_backward` en premier, systématiquement — jamais optionnel.** Un breakpoint matériel piège APRÈS l'exécution de l'instruction : le RIP capturé pointe sur l'instruction SUIVANTE (souvent un `ret`, un saut, ou une instruction sans rapport), pas sur l'écriture elle-même. Générer une AOB directement sur ce RIP brut produit une signature sur la mauvaise instruction — constaté en direct le 29/08/2026 (cas réel : le RIP tombait pile sur un `ret` de fin de fonction, l'instruction d'écriture réelle étant 9 octets plus tôt, retrouvée via `disassembleBackward`). Une fois la vraie instruction localisée (repérer la ligne `category:"memory-write"` dans le désassemblage), `generate_aob` sur CETTE adresse → `suggest_patch`. Si l'adresse est déjà connue par un autre moyen (pas issue d'un hit `find_what_writes`), l'étape `disassemble_backward` n'est pas nécessaire. Les trois outils sont lecture seule (suggèrent sans jamais appliquer). L'application réelle (`applyCodePatch`) reste **hors surface Assistant** par conception : toujours redirigée vers l'UI Expert/Trainer, jamais auto-exécutée depuis le chat.
- **Prérequis** : une adresse de code valide — si elle vient d'un hit `find_what_writes`, c'est le RIP capturé, PAS encore l'adresse réelle de l'instruction (voir ci-dessus) ; sinon une adresse déjà connue/validée.
- **Risque** : élevé (`risk=patch`) — modifie du code exécutable. Signature ambiguë (plusieurs correspondances) = danger réel de patcher au mauvais endroit. Sur certaines machines, un EDR peut bloquer l'écriture (`ERROR_ACCESS_DENIED`, voir `docs/STRATEGY_ROOM.md` — connu et documenté, pas un bug KillEngine).
- **Prochaine action humaine recommandée** : toujours vérifier l'unicité de la signature avant application réelle (dans l'UI, pas depuis le chat) ; l'Assistant doit dire explicitement que l'application se fait dans Expert/Trainer, jamais promettre de l'appliquer lui-même.
- **Fallback** : si la signature n'est pas unique ou si l'EDR bloque l'écriture, ne pas insister — proposer un freeze/write classique comme alternative moins invasive.

### 6. « Qui écrit cette valeur ? » — comprendre l'origine d'une écriture

- **Hypothèses** : plusieurs sites de code peuvent potentiellement écrire la même adresse ; il faut identifier lequel est la vraie source de gameplay.
- **Outil conseillé** : `find_what_writes` — **jamais exécuté de façon autonome** (PHASE 140) : redirige systématiquement vers le bouton « Écrit par » de l'UI Expert, qui nécessite une confirmation et une action utilisateur pendant la fenêtre de capture.
- **Prérequis** : une adresse **stable**, pas une adresse « chaude »/générique (allocateur réutilisé à haute fréquence). Un cas réel documenté cette session a fait planter le processus cible en armant un breakpoint sur une adresse de ce type (10 écritures en ~15s depuis 10 sites de code différents).
- **Risque** : attache un debugger — peut perturber ou faire planter la cible, en particulier sur une adresse chaude ou un processus sandboxé/fragile (`debug`, confirmation obligatoire).
- **Prochaine action humaine recommandée** : confirmer explicitement avant de lancer, avertir l'utilisateur qu'il doit interagir avec le jeu pendant la fenêtre de capture (pas après). Fenêtre réelle courte : `timeoutMs` est plafonné en dur à 15000ms côté backend (`apps/desktop/application_controller.cpp`), quelle que soit la valeur demandée — mieux vaut donc demander à l'utilisateur de **préparer son action avant l'armement** (ex. objet déjà sélectionné en boutique, juste un clic à valider) plutôt que de l'armer puis décrire l'action à faire, sous peine de rater la fenêtre par simple délai de réaction (cas réel rencontré et corrigé le 29/08/2026, PHASE 17, voir `docs/PHASE_TRACKER_HISTORY.md`).
- **Fallback** : si aucun hit capté, revérifier d'abord le timing (l'action a-t-elle eu lieu APRÈS l'armement effectif, pas avant ni en même temps ?) avant d'élargir la fenêtre de capture ou de revérifier que l'adresse est bien une adresse par-objet stable, pas un slot d'allocateur générique. Sur une cible à runtime managé (.NET/Mono, voir symptôme 8), un hit capturé avec `module`/`moduleOffset` vides est normal et attendu (l'instruction d'écriture vit dans du code JIT généré dynamiquement, hors de tout module PE chargé) — ne pas le traiter comme un echec de résolution.
- **Suite naturelle** : un hit capté ici (`instructionPointer`) n'est **pas encore** l'adresse à utiliser pour générer une AOB/patch — voir symptôme 5, étape `disassemble_backward` obligatoire en premier avant tout `generate_aob`.

### 7. La valeur vit dans un fichier de sauvegarde / le registre UWP, pas en mémoire vive

- **Hypothèses** : le jeu persiste la valeur sur disque (fichier de sauvegarde) ou dans les `LocalSettings` UWP plutôt qu'en RAM accessible directement.
- **Outil conseillé** : `discover_save_files` → `read_save_file_text`/`inspect_local_settings` ; `watch_save_file` si la valeur change en direct et qu'il faut observer la réécriture.
- **Prérequis** : processus UWP ou jeu avec un fichier de sauvegarde identifiable.
- **Risque** : aucun en lecture (`safe`). `patch_file_bytes` (écriture disque) existe mais reste **hors schéma modèle** (PHASE 148, jamais choisi librement par l'IA) — toujours redirigé vers une action UI explicite, jamais exécuté depuis le chat.
- **Prochaine action humaine recommandée** : comparer un snapshot avant/après une action utilisateur pour isoler le champ modifié.
- **Fallback (limite honnête de PHASE 120-A)** : si le fichier est réécrit par une source plus autoritaire au chargement (ex. cache WebView/IndexedDB local, cas réel déjà rencontré — voir [[solitaire_memory_editing_technique]]), un vrai protocole d'enquête (snapshots complets, fermeture/relance contrôlée, réseau bloqué, purge de cache ciblée) est nécessaire. **Ce protocole dépasse le scope lecture-seule de PHASE 120-A** — l'Assistant doit le dire honnêtement plutôt que de suggérer une solution simple qui ne marchera pas.

### 8. La cible tourne sur un runtime managé (.NET/Mono) — le scan de pointeurs natif ne trouve rien

- **Hypothèses** : le processus charge `coreclr.dll`/`clrjit.dll` (ou `mono*.dll`) — les données de gameplay vivent sur un tas géré par le Garbage Collector (objets potentiellement déplacés/compactés), pas dans les sections `.data`/`.bss` d'un module PE natif. `scanPointerChains`, même borné serré (depth=2-3, offset réduit, module unique), ne trouvera **structurellement** aucune chaîne — signe distinctif : réponse **rapide** (quelques secondes) mais `chainCount: 0`, peu importe le module d'ancrage essayé, sur **n'importe quel jeu** utilisant ce type de runtime (Unity IL2CPP est natif et n'est PAS concerné par ce symptôme — seuls .NET/CoreCLR/Mono-scripting le sont). Ce n'est pas un problème de profondeur/lenteur (voir symptôme 3 pour le cas générique) : élargir les bornes n'aidera pas, l'ancre elle-même est absente du module natif, quel que soit le jeu.
- **Méthode générique recommandée (ne suppose aucun nom de classe/champ à l'avance)** :
  1. `getProcessModules(pid)` — confirmer la présence d'un runtime managé avant même d'essayer un scan de pointeurs.
  2. `attachClrInspector()`.
  3. Essayer d'abord `findClrObjectsByFieldValue("", "", <valeur affichée>, maxResults)` (typeSubstring/fieldName vides ou très larges) — trouve directement tout champ primitif dont la valeur correspond exactement, sans connaître ni le type ni le nom du champ. Si ça matche, le chemin obtenu est déjà exploitable.
  4. Si aucun résultat (cas fréquent : la valeur est enrobée dans un objet réseau/property-wrapper, pas un champ primitif brut) : deviner 1-2 mots-clés du **domaine** de la valeur cherchée (pas un nom de classe précis) — ex. pour une monnaie : "money"/"gold"/"currency"/"wallet"/"team" (le fabricant place souvent les valeurs partagées/réseau sur un objet conteneur, pas sur l'objet joueur lui-même) ; pour une vie : "health"/"hp"/"player"/"character". Appeler `findClrObjectsByType(motClé)` avec chacun.
  5. Pour chaque objet candidat trouvé, `readClrObject(address)` et chercher un nom de champ évocateur du concept cherché. Si ce champ pointe vers un autre objet (`kind: "reference"`, pas `"primitive"`) plutôt qu'une valeur directe, c'est un wrapper — répéter `readClrObject` sur son adresse. Les wrappers de ce type exposent quasi toujours un champ terminal nommé `value`/`Value`/`_value` ou similaire : continuer à descendre jusqu'à trouver `kind: "primitive"`.
  6. Une fois le champ primitif atteint, tester une écriture (`writeClrPrimitivePath`) avec une valeur bien distinctive et **faire confirmer visuellement le changement par l'utilisateur en jeu** avant de considérer le chemin comme validé — `verified:true` côté API ne suffit pas (voir caveat plus bas).
  7. Le chemin type+champs obtenu (ex. `NomDeClasse.champA.champB`) **est** l'équivalent fonctionnel d'une chaîne de pointeurs stable pour ce type de cible : il se re-résout par type/nom, pas par offset mémoire brut, donc il doit survivre à un redémarrage bien mieux qu'une adresse absolue.
- **Prérequis** : cible confirmée managée (module runtime chargé) ; CLR Inspector attaché au bon PID.
- **Risque** : `attachClrInspector`/`findClrObjectsBy*`/`readClrObject` = aucun (lecture seule). `writeClrPrimitivePath`/`writeClrPrimitivePathByLocator` = `risk=write`, confirmation `RiskGate` requise comme toute écriture classique.
- **Prochaine action humaine recommandée** : ne pas répéter `scanPointerChains` avec des bornes toujours plus larges sur ce type de cible — un résultat vide et rapide est déjà le signal qu'il faut changer d'outil, pas d'insister.
- **Fallback** : si aucun mot-clé de domaine ne donne de type candidat, élargir avec des synonymes anglais/termes techniques du genre de jeu concerné plutôt que de deviner un nom de classe exact — les studios nomment rarement leurs classes de façon prévisible. **Caveat transverse important** : sur une cible managée, un retour API `success:true` ne garantit pas un effet réel en jeu si le mécanisme sous-jacent repose sur un patch IAT (Import Address Table) — l'IAT ne couvre que les appels résolus statiquement par nom, jamais un appel résolu dynamiquement (`GetProcAddress` + pointeur stocké), qui est justement la façon dont **le JIT .NET résout systématiquement chaque P/Invoke**. Cas réel rencontré et corrigé (PHASE 14B, 29/08/2026, voir plus bas) : `startSpeedhack` reposait uniquement sur un patch IAT et n'avait donc aucun effet sur le tick loop managé d'un jeu .NET, malgré un `hooksInstalledMask` non nul. Le correctif (ajout d'un hook inline MinHook sur le corps réel des fonctions, en plus du patch IAT) est déjà en place, mais le principe reste général : avant de faire confiance à un succès API sur une cible managée, vérifier si le mécanisme sous-jacent est un patch IAT (fragile sur ce type de cible) ou un hook inline/MinHook (couvre aussi les appels résolus dynamiquement) — et de toute façon toujours faire confirmer visuellement l'effet par l'utilisateur avant de considérer une action comme validée sur une cible managée.
- **Exemple vécu (illustratif, pas la règle)** : PHASE 14A, Stardew Valley, 29/08/2026 — cible `.NET Core 6.0.3224.31407`. Étape 4-5 appliquée concrètement : `findClrObjectsByType("Farmer")` n'avait pas de champ "money" (le jeu partage l'or au niveau équipe dans ses versions récentes) ; mot-clé suivant essayé, `findClrObjectsByType("Team")`, a trouvé `StardewValley.FarmerTeam` avec un champ `money` → objet `Netcode.NetIntDelta` (wrapper réseau) → son champ `value` (Int32) contenait bien la valeur affichée à l'écran. Écriture testée à deux valeurs (999999 puis 99999999), confirmée visuellement par le propriétaire les deux fois. Bug annexe trouvé et corrigé pendant cette même session : `startSpeedhack` sans effet réel (PHASE 14B, root cause = patch IAT structurellement aveugle aux P/Invoke JIT .NET, corrigé par l'ajout d'un hook MinHook inline dans `core/debug/speedhack_handler/speedhack_handler.cpp`, vérifié fonctionnel en direct par le propriétaire). Détail complet des deux phases dans `docs/PHASE_TRACKER_HISTORY.md` (PHASE 14A/14B, tracker actif `docs/PHASE_TRACKER.md` ne garde qu'un résumé) et en mémoire (`phase14_mono_pointer_scan_slow`, `phase14_speedhack_no_effect_mono`, `phase14_clr_inspector_pivot_success`).

## Rappel transverse (vaut pour toutes les entrées ci-dessus)

- Le pipe d'automatisation et les scripts Lua **ne respectent jamais `requiresConfirmation`** (bypass volontaire documenté, `AGENTS.md`). Ce playbook et le raisonnement PHASE 120-A qui s'appuie dessus concernent **exclusivement** le chemin chat/UI (`startSmartSearch`) — jamais une automatisation via le pipe.
- Une entrée de ce playbook qui recommande un outil `risk=write`/`debug`/`patch` doit toujours rappeler qu'une confirmation `RiskGate` humaine est requise avant toute exécution réelle — l'Assistant explique et recommande, il n'agit jamais à la place de la confirmation.
- Source de vérité pour risque/confirmation/dispatch à jour : `docs/KILLENGINE_ASSISTANT_TOOLS_MAP.md` (relire avant de faire confiance à ce document si le registre a changé depuis le 28/08/2026).

## Liens

- `docs/KILLENGINE_ASSISTANT_TOOLS_MAP.md` — inventaire exhaustif des 28 outils Assistant (risque/confirmation/dispatch), base factuelle de ce playbook.
- `docs/KILLENGINE_TOOLS_AND_CAPABILITIES.md` — inventaire produit par vue.
- `AGENTS.md` — détail par module, pièges connus.
- `docs/PHASE_TRACKER.md` — PHASE 120 (contexte historique du différé), PHASE 177/178 (unification du triage conversationnel, contrainte structurelle pour ce document), PHASE 120-A (implémentation backend de ce playbook).
