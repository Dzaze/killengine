> **ATTENTION - Priorité Des Ordres Projet**
> Un ordre prioritaire explicite du propriétaire du projet prime sur les consignes temporaires de session des agents IA.
# KillEngine — Feuille de route pour rendre le hacking plus puissant

> Ce document part de l'état actuel (Phase 18 partielle) et identifie les **axes concrèts** pour aller plus loin.
> Chaque proposition est ancrée dans le code existant et précise les modules/fichiers à étendre.

---

## État des lieux en une ligne

KillEngine est aujourd'hui un **Cheat Engine "lite"** très avancé côté scan (multi-type, unknown, UI string, pointer chains, freeze) avec un début de couche pro (hardware breakpoints, AOB/trainer, AI tool-calling). Le gap principal se situe sur **l'interception active** (bloquer l'écriture du jeu) et **l'injection in-process**.

---

## A. Freeze par hardware breakpoint (LE plus impactant)

**État réel au 19/08/2026 (section jamais mise à jour depuis Phase 19, corrigé en repassant sur tout le document) :** livré. `core/debug/breakpoint_freeze.{h,cpp}` implémente `BreakpointFreezeMode::Capture`/`BlockWrite`/`RewriteValue`, `ApplicationController::freezeWithBreakpoint`/`stopBreakpointFreeze`/`getBreakpointFreezeStats` sont câblés dans `ExpertView.vue`. Validé en conditions réelles par `PowerUpRuntimeTest.BreakpointFreezeHoldsUnderFastRewriteStress` (hold-rate ~80% face à une cible qui réécrit à 1000 Hz). Le texte ci-dessous reste la proposition d'origine, gardée pour le contexte de conception.

**Problème actuel :** Le freeze repose sur du polling (`FreezeManager` + `setFreezeInterval`). Même à 16 ms, le jeu SC2 réécrit la valeur entre deux ticks → clignotement.

**Solution :** Bloquer l'écriture du jeu à la source via hardware breakpoint.

**Ce qui existe :**
- `core/debug/hardware_breakpoint.cpp` — capture `EXCEPTION_SINGLE_STEP`, lit `RIP`, mais **ne modifie pas le contexte CPU**.

**Ce qu'il faut ajouter :**
1. Dans `HardwareBreakpointSession::monitorLoop`, quand un hit `Write` est capturé :
   - Option **"freeze-block"** : modifier `CONTEXT::Rax` (ou le registre destination) via `SetThreadContext` pour annuler l'écriture, ou réécrire la valeur voulue **juste après** l'instruction fautive dans le même cycle debug.
   - Option **"freeze-rewrite"** : laisser l'instruction écrire, puis immédiatement `WriteProcessMemory` la valeur figée avant de relâcher le thread.
2. Nouveau mode `BreakpointFreezeMode { Capture, BlockWrite, RewriteValue }` dans `BreakpointConfig`.
3. API backend : `freezeWithBreakpoint(address, value, type)` → pose DR0 en mode `RewriteValue` et maintient la session debugger active.

**Effort :** Moyen. **Impact :** Énorme — c'est ce qui fait tenir un freeze SC2 à 100 %.

**Fichiers touchés :**
- `core/debug/hardware_breakpoint.h/.cpp` — ajout du mode rewrite/block
- `core/freeze/freeze_manager.h/.cpp` — pont vers la session breakpoint
- `apps/desktop/application_controller.cpp` — `freezeWithBreakpoint`
- `ui/src/services/backend.ts` + `ui/src/views/ExpertView.vue`

---

## B. Injection DLL + Hooking in-process

**État réel au 19/08/2026 :** livré, `core/inject/` existe exactement comme proposé (`dll_injector.{h,cpp}`, `function_hook.{h,cpp}`) — le module `remote_shellcode.*` séparé n'a pas été créé mais son rôle est couvert par `injectShellcode()` dans `dll_injector.cpp`. `ApplicationController::injectDllIntoProcess`/`installFunctionHook`/`removeFunctionHook`, UI dans `InjectionPanel.vue`. Cas d'usage 1 (hook qui réécrit une valeur au lieu d'un patch NOP) livré via `forceWriteInstructionValue`/le bouton "Forcer valeur (hook)" dans `ExpertView.vue`. Cas d'usage 3 (interception `WriteFile`/`DrawText`) volontairement pas fait — hors du besoin memory-scanning de l'app. Validé par `PowerUpRuntimeTest.InjectDllFailsCleanlyOnMissingDll`/`InlineHookHelpersProduceValidShellcode`/`InjectShellcodeRetDoesNotCrashTarget`/`GetRemoteProcAddressFindsLoadLibraryW`, tous sur processus réel.

**Problème :** Toute la logique est out-of-process (`ReadProcessMemory`/`WriteProcessMemory`). On ne peut pas intercepter les calculs, seulement lire/écrire le résultat.

**Solution :** Module d'injection + hooking.

**Nouveau module proposé : `core/inject/`**
```
core/inject/
├── dll_injector.h/.cpp      # CreateRemoteThread + LoadLibraryW
├── remote_shellcode.h/.cpp  # Alloc + Write + Exec de shellcode (sans DLL sur disque)
└── function_hook.h/.cpp     # Inline hook (detour) : sauvegarder bytes, jumper vers trampoline
```

**Cas d'usage concrets :**
1. **Hook de l'instruction trouvée par "Find What Writes"** : au lieu de patcher en NOP, installer un detour qui réécrit la valeur voulue à chaque appel → freeze invincible sans polling.
2. **Trainer in-process** : la DLL injectée gère les toggles/keys localement, KillEngine pilote via named pipe ou shared memory.
3. **Interception de fonctions** : hook `WriteFile`, `DrawText`, etc. pour comprendre le rendering des ressources.

**Effort :** Élevé. **Impact :** Très haut — passe KillEngine au niveau WeMod/Cheat Engine.

**Risques :** Détection anti-cheat, crash du jeu, complexité. À garder en mode Expert + avertissement.

---

## C. Scan de valeurs chiffrées / obfusquées

**État réel au 19/08/2026 :** livré. `core/scanner/encrypted_scan.{h,cpp}` (pas un fichier `group_scan.*` séparé comme suggéré ci-dessous — le group scan a été plié dans le même module, `GroupScanEntry`/`GroupScanOptions`) couvre XOR/Add/Sub/NOT/Group scan. `ApplicationController::scanEncryptedValue`/`scanGroupScan` exposés côté UI.

**Problème :** Beaucoup de jeux modernes (mobiles, certains PC) stockent les valeurs XORées, additionnées d'une clé, ou obfusquées. Le scan exact échoue.

**Ce qui existe :** `core/scanner/value_variants.cpp` gère déjà les fixed-point (x10/x100/x1000/x65536) et unsigned.

**Ce qu'il faut ajouter :** `core/scanner/encrypted_scan.h/.cpp`
```
EncryptedScanOptions {
    enum Mode { XorKey, AddKey, NotBits, CustomLambda };
    uint64_t key;
    int keySearchBits; // 8/16/32/64
};
```
1. **XOR scan** : tester `value ^ key` pour une plage de clés (brute-force borné sur 8/16 bits, ou clé fournie).
2. **Add/Sub scan** : tester `value + key`, `value - key`.
3. **NOT scan** : `~value`.
4. **Group scan** : chercher N valeurs avec offsets fixes connus (ex: HP/Mana/Stamina côte à côte).

**Effort :** Moyen. **Impact :** Débloque toute une catégorie de jeux protégés.

---

## D. Analyseur de structures (Memory Dissect)

**État réel au 19/08/2026 :** livré pour 1-3. `core/scanner/structure_analyzer.{h,cpp}` — dissect view (champs typés décodés depuis une fenêtre mémoire) et diff automatique (`StructureAnalyzer.DiffMarksChangedFieldsWithRealFieldSizes`, testé). Point 4 (template de structure réutilisable) livré aussi mais pas dans `ProfileStore` comme suggéré ci-dessous : stocké séparément en `localStorage` frontend (`structureTemplateStorageKey`, `ExpertView.vue`/`app.ts`), pas persisté côté backend/`.keprofile`. Le point 3 (déduction du delta d'offset entre deux instances de la même entité) n'a pas d'automatisation dédiée — l'utilisateur compare les deux dissections manuellement.

**Problème :** Quand on trouve la valeur HP d'une unité, ses voisins (Mana, Position X/Y, Owner) sont à côté, mais l'utilisateur doit deviner les offsets.

**Ce qui existe :** Analyse de structure v1 (`AGENTS.md` mentionne "lectures autour d'une source sérieuse, voisins Int32/Float32").

**Ce qu'il faut ajouter :** `core/scanner/structure_analyzer.h/.cpp`
1. **Dissect view** : depuis une adresse, lire une fenêtre (ex: 256 octets) et décoder tous les champs typés (Int8/16/32/64, Float32/64, pointer, ASCII).
2. **Diff automatique** : capturer la fenêtre, faire varier une valeur dans le jeu, recomparer → les offsets modifiés sont surlignés.
3. **Déduction de structure** : si l'utilisateur trouve HP pour le joueur 1 et joueur 2, calculer le delta d'offset → réutiliser pour scanner tous les joueurs.
4. **Template de structure réutilisable** : sauvegarder dans le profil (`ProfileStore`) un "layout" avec offsets nommés.

**Effort :** Moyen. **Impact :** Très utile pour les jeux avec entités (RPG, RTS, MOBA).

---

## E. Scripting / Auto-Assembler

**État réel au 18/08/2026 (cette section décrivait encore l'état "à faire" alors que 1-3 sont maintenant livrés) :**
- **Mini-langage type Cheat Engine** : `core/scripting/auto_assembler.{h,cpp}` parse `alloc()`/`label()`/`"nom:"`/`"module"+offset:`/`mov [reg+disp], imm`/`jmp`/`je`/`jne`/`call`/`ret`/`nop`/`int3`/`db`/`de`/`dd` — le script d'exemple ci-dessus (alloc+label+newmem+site existant+exit) compile et s'exécute réellement tel quel.
- **Assembleur runtime** : pas de Keystone — encodeur x64 maison volontairement borné (`encodeMemImmMov` pour l'immédiat mémoire, encodage manuel des jmp/call rel32). Couvre le sous-ensemble ci-dessus, refuse proprement le reste (adressage indexé, RIP-relatif, `add`/`sub`/`cmp`/`push`/`pop` pas encore reconnus par le parseur).
- **Exécution** : `executeAutoAsmScript` alloue, résout les modules du processus attaché (`ProcessEnumerator::enumerateModules`), compile en une région par changement de curseur (bloc alloué **ou** site existant patché en place), écrit chaque région avec restauration best-effort atomique si une région échoue après que d'autres ont réussi. `restoreAutoAsmScript` restaure tout et libère les allocations.
- Testé : `tests/unit/test_power_up_modules.cpp` (`AutoAssembler.*`, 11 cas dont un pattern CE deux-régions complet vérifié octet par octet).

**Ce qu'il reste à faire :**
4. ~~**Sauvegarde** : le script devient un type de patch dans `ProfileStore`, rejouable sans recoller le texte à chaque session~~ **Fait le 19/08/2026** : nouveau `ProfileAutoAsmScript` (`core/profiles/profile_store.h`) persisté dans le `.keprofile` au même titre que `ProfileCodePatch` ; `ApplicationController::saveProfileAutoAsmScript`/`applyProfileAutoAsmScript`/`deleteProfileAutoAsmScript` (valide le script avec `parseAutoAsmScript` avant sauvegarde, l'exécution réutilise `executeAutoAssemblerScript` tel quel). UI : liste des scripts sauvegardés dans `InjectionPanel.vue` (Charger/Exécuter/Supprimer).

**Effort restant :** Aucun — les 4 points de cette section sont livrés.

---

## F. Instrumentation sans canal de debug Win32 exclusif

**Problème :** `DebugActiveProcess` (utilisé par `hardware_breakpoint.cpp`) est un canal exclusif — un seul débogueur peut le posséder à la fois. Si l'utilisateur débogue déjà sa cible avec un autre outil, ou si la cible refuse tout attachement de débogueur, le hardware breakpoint externe devient inutilisable. Voir `docs/ULTIMATE_PRODUCT_GUIDELINE.md` section "Principe De Conception Dual-Use" pour la règle de construction qui encadre ce type de feature (ne pas nommer/cibler un produit anti-cheat précis, garder l'implémentation générique).

**Alternatives en cascade (du moins au plus intrusif) :**

| Niveau | Technique | Dépend du canal de debug Win32 | Implémentation |
|--------|-----------|-----------|----------------|
| 0 | Polling + VirtualProtectEx | Non | ✅ Déjà fait |
| 1 | Page Guards (`PAGE_GUARD`) | Non | ✅ Fait le 19/08/2026 |
| 2 | Hardware breakpoint posé depuis l'intérieur de la cible (composant injecté, VEH + registres DR0-DR7) | Non | ✅ Fait le 19-20/08/2026 |
| 3 | Kernel driver (`\\.\KillEngineKernel`) | Non | ✅ Fait le 20/08/2026 (PHASE 39) — lecture/écriture mémoire réelles via `KeStackAttachProcess`/`ProbeForRead`/`ProbeForWrite`, pas seulement le health-probe initial ; câblé nativement à l'Assistant et au mode Expert le 20/08/2026 (PHASE 42) |
| 4 | DMA hardware (carte PCIe) | Non | Hors scope |

**Fait le 19/08/2026 (niveau 1) : `core/debug/page_guard.{h,cpp}` + `core/debug/page_guard_handler/page_guard_handler.cpp`**
- Le point critique documenté ci-dessus ("le VEH doit vivre dans le processus cible") est résolu : nouvelle DLL autonome `KillEnginePageGuardHandler.dll` (sans dépendance Qt/killcore, cible CMake séparée), injectée via `killcore::injectDll` puis installant elle-même `AddVectoredExceptionHandler` + `VirtualProtect(PAGE_GUARD)` **dans** le processus cible.
- IPC par mémoire partagée nommée (`core/debug/page_guard_ipc.h`, struct POD `PageGuardIpcState`) : KillEngine écrit l'adresse/taille à surveiller avant l'injection, la DLL y écrit chaque hit (RIP, adresse exacte, type d'accès), KillEngine sonde par polling.
- Re-armement one-shot via trap flag (single-step) après chaque violation, exactement le même mécanisme que la version in-process déjà présente dans `page_guard.cpp` (réutilisé, pas réinventé).
- `ApplicationController::startPageGuardWatchAsync`/`cancelPageGuardWatch` (même patron que `findWhatWritesAsync`), exposés dans Expert (`ExpertView.vue`, bouton "Écrit par (sans debugger)").
- **Validé en conditions réelles**, pas seulement en théorie : nouveau test d'intégration `PowerUpRuntimeTest.PageGuardCapturesRemoteStressRewrite` injecte le vrai handler dans `KillEngineTestTarget.exe` pendant qu'il réécrit sa mémoire à ~1000 Hz, et vérifie un hit capturé avec le bon RIP/module — sans `SeDebugPrivilege`, contrairement au test breakpoint freeze équivalent.

**Fait le 19-20/08/2026 (niveau 2) : `core/debug/inprocess_breakpoint.{h,cpp}` + `core/debug/inprocess_breakpoint_handler/inprocess_breakpoint_handler.cpp`** — voir `docs/PHASE_TRACKER.md` PHASE 27 pour le journal complet (deux crashes réels rencontrés et corrigés en cours de route, architecture finale et limitations acceptées documentées en détail là-bas). Résumé : composant injecté qui pose lui-même un hardware breakpoint (DR0 + VEH) depuis l'intérieur de la cible, plus précis qu'un Page Guard (adresse exacte plutôt que page de 4 Ko), sans dépendre du canal de debug Win32 exclusif. Limitations v1 acceptées : seules les threads créées après l'injection sont couvertes, et une seule capture/freeze par cible et par lancement de KillEngine.

**Effort :** Niveaux 0-2 faits. **Impact :** Débloque l'instrumentation par breakpoint quand le canal de debug Win32 est indisponible (déjà pris par un autre outil, ou refusé par la cible).

---

## G. Hotkeys globales + Trainer overlay

**État réel au 19/08/2026 :** les deux briques (1 et 2) existaient déjà avant cette entrée — `core/input/global_hotkey.{h,cpp}` (hotkeys par feature Trainer, `toggle_freeze`/`toggle_patch`/`write_value`) et l'overlay Trainer lui-même (`ApplicationController::setTrainerOverlayVisible`/`updateTrainerOverlay`, bouton "Overlay ON/OFF" dans `TrainerView.vue`). Le vrai trou, trouvé en vérifiant plutôt qu'en recodant à l'aveugle : le type de hotkey `ToggleOverlay` existait déjà côté backend (`registerGlobalHotkey({type:'toggle_overlay',...})` fonctionnel) mais **rien côté frontend ne l'utilisait ni ne réagissait à l'événement** — `handleGlobalHotkey` (`app.ts`) ne savait traiter que les hotkeys liées à une `trainerFeature`.

**Fait le 19/08/2026 :**
- `handleGlobalHotkey` reconnaît maintenant `event.type === 'toggle_overlay'` et appelle `setTrainerOverlay(!trainerOverlayVisible)`.
- Nouveau champ dans `TrainerView.vue` pour enregistrer/retirer une hotkey dédiée à l'overlay (`registerOverlayHotkey`/`unregisterOverlayHotkey`, persistée en `localStorage`).
- **Bug de fond corrigé au passage** : `GlobalHotkeyManager` (backend) repart à zéro à chaque lancement de KillEngine — les hotkeys enregistrées lors d'une session précédente (features **et** overlay) semblaient toujours actives dans l'UI après redémarrage mais ne déclenchaient plus rien tant que l'utilisateur ne les reconfigurait pas manuellement. Nouveau `reregisterPersistedHotkeys()`, appelé à l'init, re-enregistre tout ce qui est persisté contre le gestionnaire fraîchement recréé.

**Effort :** Fait. **Impact :** UX de trainer pro, utilisable en plein écran — plus besoin d'alt-tab pour l'overlay, et les hotkeys survivent à un redémarrage.

---

## H. AI proactive + Auto-stratégie

**État réel au 16/08/2026, mis à jour après le nettoyage du même jour :** le workflow décrit plus bas dans une version précédente de ce document existe maintenant réellement, avec une seule implémentation par bout de logique.

- `ApplicationController::startAutoResolve` (`apps/desktop/application_controller.cpp`, ~350 lignes) reste le moteur de la phase "trouver la valeur" : scan exact multi-type → fallback scan chiffré XOR borné → fallback Trace UI string borné → capture Unknown bornée, avec `executedSafeSteps` audité et confirmation obligatoire avant tout write/freeze.
- **Chaînage réel écrit → find-what-writes → AOB → patch** : dès qu'une capture Find What Writes réussit (bouton `Écrit par` dans Expert), la signature AOB et les suggestions de patch se génèrent automatiquement (lecture seule, `autoChainFindWhatWritesResult` dans `ExpertView.vue`, réutilise le même chemin que le clic manuel "Analyser"). Sauvegarder en Trainer ou appliquer un patch restent des clics explicites — jamais automatiques.
- **Persistance après écriture** : `suggestStableLocatorForAddress` cherche une chaîne de pointeurs stable après une écriture confirmée ; le store frontend le déclenche automatiquement en fond (silencieux si rien trouvé, dédupliqué par adresse) après chaque écriture Expert réussie.
- **Nettoyage du doublon `AutoResolver`/`summarizeAutoTelemetry`** : les deux implémentations mortes ou dupliquées ont été retirées. `killai::computeAutoResolveTelemetryReport` est maintenant la seule fonction qui analyse la télémétrie et produit les insights actionnables ; `getAutoResolveReport` l'appelle directement au lieu de la réimplémenter en ligne. Testée dans `tests/unit/test_auto_resolver.cpp`, y compris le cas "500 candidats après Analyser sources" qui n'avait aucune couverture avant.
- L'apprentissage par jeu existe déjà, partiellement : `rememberAutoResolverProgress` persiste des compteurs par processus (`autoResolver/process/<jeu>` dans `QSettings`) — démarrages, réductions, checkpoints bas-candidats. Pas encore de vraie mémoire de pattern structuré ("minéraux = Int32 à +0x1A4") comme décrit plus bas.

**Ce qui reste à faire, dans le prolongement de ce qui existe :**

1. ~~**Déclencher le chaînage automatiquement, pas seulement au clic "Écrit par".**~~ **Fait le 18/08/2026** : `ApplicationController::registerWriteWatch`/`applyWriteWatchTick` (nouveau `QTimer` indépendant, 1.5s) surveille toute écriture confirmée (`writeMemoryValueConfirmed`, point d'appel unique de tous les auto-write) pendant ~12s après écriture ; si la valeur repart (2 sondages consécutifs en désaccord, anti-faux-positif), un signal `writeDidNotHold` déclenche automatiquement le chemin `find_what_writes_targets` déjà câblé côté `AssistantView.vue` — plus besoin de cliquer "Écrit par" à la main.
2. ~~**Vraie mémoire de pattern par jeu.**~~ **Fait le 19/08/2026** : le mécanisme existant (`logAiAudit`) ne gardait qu'un **seul** dernier succès écrasé à chaque fois, et surtout stockait une **adresse absolue** — inutile après un redémarrage/ASLR, le vrai bug derrière la lacune décrite ici. Corrigé : nouveau `resolveModuleOffset()` convertit l'adresse en (module, offset relatif) avant stockage ; nouvelle liste `rememberedPatterns` (JSON, plafonnée à 20, dédupliquée par module+offset avec compteur de confirmations) au lieu d'un scalaire unique. Nouveau `Q_INVOKABLE getRememberedPatterns()` résout chaque entrée en adresse live si un processus est attaché (module présent dans le process courant) — exposé dans `SettingsView.vue` avec un bouton "Prévisualiser" par entrée résolue. Pas encore de "rôle probable" auto-détecté (argent/vie/...) : le label reste l'événement d'audit d'origine (`checkpoint_write_executed`, etc.), l'inférence sémantique reste un chantier à part.
3. ~~**Rapport IA plus explicatif.**~~ **Fait le 19/08/2026** : `InvestigationView.vue` charge le rapport IA automatiquement à l'ouverture, et chaque étape/checkpoint affiche une ligne "Pourquoi : ..." corrélée au rapport (`stepStrategyReason()`/`checkpointStrategyReason()`). Voir `docs/PHASE_TRACKER.md` PHASE 23 (candidat 4).

**Hors scope, volontairement** : contournement d'anti-cheat, triche en partie multijoueur en ligne. Demandé explicitement le 16/08/2026, décliné pour la même raison que documentée dans `docs/ULTIMATE_PRODUCT_GUIDELINE.md` — construire ou documenter comment le construire revient au même risque, indépendamment de qui écrit le code ensuite. L'angle légitime et commercialement valable reste l'outillage QA/sécurité pour des développeurs testant leur propre build en environnement contrôlé, qui n'a besoin d'aucun contournement puisque c'est leur propre logiciel.

**Effort :** Fait — chaînage aval (find-what-writes → patch), nettoyage de duplication, déclenchement amont (write-watch), mémoire de pattern par jeu, et rapport IA relié à la timeline Investigation (points 1-3 ci-dessus tous livrés). **Impact :** Différenciateur majeur vs Cheat Engine.

---

## I. Améliorations transverses "quick wins"

**État vérifié le 19/08/2026** (cette table n'avait jamais été recroisée avec le code réel) :

| Amélioration | Module | État | Impact |
|--------------|--------|------|--------|
| **Scan groupé** (plusieurs valeurs proches) | `core/scanner/encrypted_scan.{h,cpp}` (`GroupScanEntry`/`GroupScanOptions`, pas de fichier séparé) | ✅ Fait — `scanGroupScan` | Trouve les structures rapidement |
| **Scan différenciel rapide** (snapshot A vs B) | `core/snapshot/snapshot_store.cpp` | ✅ Probablement déjà couvert — `memcmp` en page-skip avant comparaison valeur par valeur, pas du XOR SIMD explicite mais même objectif (sauter vite les pages inchangées) | Unknown scan plus rapide |
| **Dump mémoire → fichier** (par région, taille au choix) | `apps/desktop/application_controller.cpp` (`dumpMemoryRegion`) | ✅ Fait — `MemoryView.vue`, sélecteur 256 o à 1 Mo | Debug/expertise offline |
| **Recherche de chaînes de pointeurs depuis breakpoint** | `apps/desktop/application_controller.cpp` (`suggestStableLocatorForAddress`) | ✅ Fait — déclenché automatiquement en fond après une écriture Expert confirmée | Remonte à la base automatiquement |
| **Watch expressions** (adresse + offset + formule) | `ui/src/stores/app.ts` (`WatchedPointerChain`), `PointerChainWatchPanel.vue` | ✅ Fait — bouton "Watch" dans Pointer Chains, toggle Live + ajout manuel (19/08/2026) | Suit les pointeurs en live |
| **Édition hex inline** dans l'inspecteur mémoire | `ui/src/views/MemoryView.vue` | ✅ Fait — édition ligne par ligne + vue hexadécimale paginée (19/08/2026) | UX power-user |
| **Filtres de région avancés** (par module, par commit charge) | `ui/src/views/MemoryView.vue` | ✅ Fait — filtre par nom de module ajouté (19/08/2026), en plus d'état/lisible/writable/exécutable | Moins de bruit |
| **Historique d'écritures avec replay** | `apps/desktop/application_controller.cpp` (`persistWriteHistorySequenceEntry`) | ✅ Fait — séquence persistée `QSettings` par jeu, `replayWriteHistorySequence` (19/08/2026), distinct du rollback en session | Audit/debugging |
| **Écriture multi-adresses simultanée/atomique** | `core/process/process_suspend.*`, `writeMemoryValuesAtomic` | ✅ Fait — bouton "Écrire ensemble (atomique)" + `InfoDot` dans le panneau Candidats d'Expert (19/08/2026), en plus du connecteur d'automatisation | Contourne les cibles à copies redondantes |
| **Next scan "entre deux valeurs"** (range) | `core/scanner/scan_types.cpp` (`NextScanMode`) | ❌ Absent — vérifié le 20/08/2026 : seuls `exact`/`changed`/`unchanged`/`increased`/`decreased`/`delta` existent, pas de mode plage | Cible une plage (ex: HP entre 50 et 100) sans deux next scans successifs (`>= min` puis `<= max`) |
| **Résolution de symboles par nom** (ex: `kernel32.dll!CreateFileW` → adresse) | `apps/desktop/application_controller.cpp` (`getProcessModules`) | ❌ Absent — vérifié le 20/08/2026 : résolution actuelle uniquement adresse→module+offset (via la liste de modules), pas de table d'exports nom→adresse | Cibler directement une fonction connue (hook, breakpoint) sans passer par Find What Writes |

---

## J. Speedhack (accélérer/ralentir le temps perçu d'un processus)

**État réel au 20/08/2026 :** absent — vérifié par grep sur tout le repo (`speedhack`, `SetSystemTimeAdjustment`, `timeScale`), aucun résultat. Identifié en comparant KillEngine à la liste de fonctionnalités attendues d'un trainer complet (WeMod/Cheat Engine la proposent quasi systématiquement).

**Problème :** Beaucoup de jeux basent cooldowns/animations/physique sur `QueryPerformanceCounter`/`GetTickCount`/`GetTickCount64`/`timeGetTime` lus par le process cible lui-même — ralentir ou accélérer les valeurs retournées change la vitesse perçue du jeu sans toucher à sa logique métier.

**Ce qui existe déjà et serait réutilisé tel quel :** `core/inject/function_hook.{h,cpp}` (inline hook/detour générique), et le patron de composant injecté + IPC mémoire partagée déjà livré deux fois (`KillEnginePageGuardHandler.dll`, `core/debug/page_guard_ipc.h` ; `KillEngineInProcessBreakpointHandler.dll`) — seule la fonction hookée et le rôle de l'IPC changent.

**Ce qu'il faudrait ajouter :**
1. Nouveau composant injecté (même patron CMake séparé sans dépendance Qt/killcore) qui hooke `kernel32.dll!QueryPerformanceCounter`/`GetTickCount`/`GetTickCount64`, `winmm.dll!timeGetTime` **dans** le process cible, et multiplie la valeur retournée par un facteur lu depuis la mémoire partagée.
2. IPC mémoire partagée nommée pour piloter le facteur depuis KillEngine sans réinjecter à chaque changement (même patron que `page_guard_ipc.h`).
3. UI : slider de facteur (0.1x-10x) + preset "pause" dans Expert ou Trainer.

**Effort :** Moyen — réutilise l'injection/hooking déjà livrés ; la nouveauté est le hook des fonctions de temps + l'IPC de facteur. **Impact :** Feature très demandée côté trainers grand public, absente aujourd'hui.

**Fichiers touchés (proposés) :** nouveau `core/speedhack/` (composant injecté + IPC), `core/inject/dll_injector.cpp` (réutilisé), `apps/desktop/application_controller.cpp`, `ui/src/views/ExpertView.vue`/`TrainerView.vue`.

---

## K. Lua scripting

**État réel au 20/08/2026 :** absent — vérifié par grep (`lua_state`, `luaL_`), zéro référence dans le repo. L'automatisation actuelle passe uniquement par l'Auto-Assembler (DSL propre, volontairement borné — voir section E) et par l'IA locale (tool-calling JSON, pas un langage général).

**Problème :** Cheat Engine expose Lua pour scripter des workflows arbitraires (UI custom, logique conditionnelle, orchestration de plusieurs actions). KillEngine n'a pas d'équivalent — un utilisateur qui dépasse ce que l'Auto-Assembler borné permet (pas d'`add`/`sub`/`cmp`/boucles) n'a aucun recours scriptable.

**Ce qui existe :** le pipe d'automatisation JSON-RPC déjà livré (`automation_pipe_server.h`) est le point d'ancrage le plus proche — un script Lua pourrait piloter KillEngine via ce même pipe plutôt que d'être embarqué in-process.

**Ce qu'il faudrait ajouter :**
1. Décision d'architecture d'abord : Lua **embarqué** (lier `lua5.4`/LuaJIT, bindings C vers les mêmes primitives qu'`ApplicationController`) vs Lua **externe** pilotant le pipe d'automatisation existant (plus simple, plus sûr, latence IPC au lieu d'appels directs — probablement le meilleur point de départ).
2. Si embarqué : nouveau `core/scripting/lua_runtime.{h,cpp}`, bindings vers `readMemoryPreview`/`writeMemoryValue`/`startExactScan`/etc.
3. UI : éditeur de script (même patron que le champ Auto-Assembler dans `InjectionPanel.vue`), Exécuter/Arrêter, logs.

**Effort :** Élevé — nouvelle dépendance runtime, surface de sécurité à border (un script a accès à tout ce qu'expose l'API), UI d'édition/débogage. **Impact :** Rapproche KillEngine de la flexibilité de scripting de Cheat Engine.

**Fichiers touchés (proposés) :** nouveau `core/scripting/lua_runtime.{h,cpp}` (ou wrapper externe autour du pipe existant), `apps/desktop/application_controller.h/.cpp`, nouvelle vue `ui/src/views/ScriptingView.vue`.

---

## L. Pointer maps / rescans après redémarrage

**État réel au 20/08/2026 :** absent comme fonctionnalité dédiée — `scanPointerChains` retrouve une chaîne stable pour une session donnée, `ProfileStore` persiste des `ProfileTarget` avec chaînes résolues, mais rien ne compare **plusieurs** chaînes à la fois avant/après un redémarrage (pattern Cheat Engine "pointer map"/fichier `.PTR`).

**Problème :** Après un redémarrage du jeu (nouvelle base ASLR), l'utilisateur doit revalider chaque chaîne de pointeurs une par une, sans diagnostic groupé ("ces 3 chaînes sur 5 restent valides, ces 2 ont changé").

**Ce qui existe et serait réutilisé :** `core/profiles/profile_store.{h,cpp}` (`ProfileTarget`, `resolveProfileTarget`), `suggestStableLocatorForAddress` (déjà déclenché automatiquement après écriture confirmée).

**Ce qu'il faudrait ajouter :**
1. `ApplicationController::comparePointerMapAcrossRestart(profileName)` — relit chaque `ProfileTarget` du profil sur le process actuellement attaché, marque chaque chaîne valide/invalide.
2. UI : vue tableau dans `ProfileView.vue` (cible/statut avant/statut après/action garder-ou-rescanner).
3. Optionnel : export/import de map de pointeurs en texte simple pour partage entre utilisateurs.

**Effort :** Moyen — réutilise `ProfileStore`/`resolveProfileTarget` existants ; la nouveauté est la comparaison groupée + l'UI dédiée. **Impact :** Évite de tout rescanner à l'aveugle sur une cible déjà connue.

**Fichiers touchés (proposés) :** `apps/desktop/application_controller.cpp` (nouvelle méthode), `core/profiles/profile_store.h/.cpp` (champ de statut), `ui/src/views/ProfileView.vue`.

---

## M. Cheat table avancée : dépendances entre entrées

**État réel au 20/08/2026 :** absent — chaque feature/toggle Trainer est indépendante aujourd'hui (`core/profiles/profile_store.h`, apply/restore individuels). Aucune notion de graphe de dépendances entre entrées façon Cheat Engine (ex: "active Y automatiquement quand X est activé").

**Problème :** Pour une cheat table complexe (ex: "God Mode" qui doit activer Infinite HP + Infinite Mana ensemble), l'utilisateur doit activer chaque toggle séparément à la main.

**Ce qu'il faudrait ajouter :**
1. Champ `dependsOn: QStringList` sur chaque feature Trainer (référence à d'autres features par nom).
2. Résolution d'ordre d'application (tri topologique simple), refuser un cycle de dépendances proprement.
3. UI : sélecteur de dépendances dans le formulaire de création de feature (`TrainerView.vue`), badge visuel sur les features qui ont des prérequis.

**Effort :** Faible à moyen — surtout de la donnée + validation, pas de nouvelle primitive mémoire. **Impact :** Confort pour les cheat tables complexes à plusieurs toggles liés.

**Fichiers touchés (proposés) :** `core/profiles/profile_store.h/.cpp`, `apps/desktop/application_controller.cpp`, `ui/src/views/TrainerView.vue`.

---

## Priorisation recommandée (impact × faisabilité)

### Phase 19 — Breakpoint freeze + Structure analyzer (gros gain, code existant)
1. ✅ **Freeze par hardware breakpoint** (A) — tient enfin sur SC2
2. ✅ **Analyseur de structures** (D) — déduit les layouts automatiquement
3. ✅ **Scan de valeurs chiffrées** (C) — débloque les jeux obfusqués
4. ✅ **Page Guards** (F) — alternative sans canal de debug Win32 — fait le 19/08/2026

### Phase 20 — Injection & Scripting (passe au niveau pro)
5. ✅ **Injection DLL + hooks** (B) — freeze invincible
6. ✅ **Auto-assembler** (E) — scripts complexes, y compris la persistance (E.4) — fait le 19/08/2026
7. ✅ **Hotkeys + overlay** (G) — UX trainer — fait le 19/08/2026

### Phase 21 — AI proactive
8. ✅ **Auto-résolution workflow** (H) — l'IA enchaîne seule (chaînage aval + amont)
9. ✅ **Apprentissage par jeu** — mémoire des patterns (module+offset, pas encore de rôle sémantique auto-détecté) — fait le 19/08/2026

---

## Prochains gros chantiers (à date du 19/08/2026)

Toutes les phases 19-21 ci-dessus sont closes. Les 6 candidats listés ci-dessous (vérifiés dans le code, pas supposés) sont tous livrés depuis le 19/08/2026 (demande explicite : "tu fais tout les chantier", détail dans `docs/PHASE_TRACKER.md` PHASE 23). Section conservée pour le contexte de conception et le prochain audit — chaque entrée doit être re-vérifiée dans le code avant de servir de base à une nouvelle demande, cette liste peut redevenir stale exactement comme les sections A-D l'étaient devenues.

1. ~~**Écrire/injecter une valeur depuis un checkpoint "Écrit par" dans Investigation**~~ **Fait le 19/08/2026** : nouvelle action `force_value` dans `buildCheckpointActionPlan` (`ui/src/stores/app.ts`, activée pour `kind === 'code_writer'`), nouveau `executeCheckpointForceValue()` qui appelle `suggestCodePatches` puis `forceWriteInstructionValue` (rien de nouveau côté C++, juste la connexion identifiée ci-dessous). Bouton "Forcer valeur (hook)" + champ valeur sur la carte checkpoint d'`InvestigationView.vue`. Voir `docs/PHASE_TRACKER.md` PHASE 23.

2. ~~**Watch expressions (adresse + offset + formule, live)**~~ **Fait le 19/08/2026** : en creusant avant de coder, la brique de résolution + le modèle de données (`WatchedPointerChain`, `addWatchedPointerChain`/`refreshWatchedPointerChain(s)`/`removeWatchedPointerChain`/`clearWatchedPointerChains` dans `ui/src/stores/app.ts`) et même le panneau d'affichage (`PointerChainWatchPanel.vue`) existaient déjà — mais `addWatchedPointerChain` n'avait **aucun appelant** dans toute l'UI (le panneau était donc en permanence vide, aucun moyen d'y ajouter une entrée), et il n'y avait pas de timer de re-résolution automatique (seulement un bouton "Rafraichir" manuel). Corrigé : bouton "Watch" ajouté à côté de Tester/Utiliser/Sauver profil/Note dans le panneau Pointer Chains d'`ExpertView.vue` (`watchPointerChain()`) ; nouveau `setWatchedPointerChainsLiveEnabled()` + timer 1s indépendant (même patron que `setWatchLiveEnabled`/`watchLiveTimer` pour les adresses fixes) ; `PointerChainWatchPanel.vue` gagne un toggle "Live ON/OFF" et un formulaire d'ajout manuel (module + offset de base + offsets, sans passer par un scan de pointeurs complet) pour une chaîne déjà connue. Voir `docs/PHASE_TRACKER.md` PHASE 23.

3. ~~**Rôle sémantique auto-détecté pour la mémoire de pattern par jeu**~~ **Fait le 19/08/2026** (option 2, inféré — tranché dans `docs/STRATEGY_ROOM.md`) : le "fil à tirer" redouté existait déjà — le wrapper `logAiAudit()` (`ui/src/stores/app.ts`) injecte `objective: activeInvestigation.value?.objective ?? searchQuery.value` sur chaque appel, sans exception. Il suffisait de le lire côté C++ : `ApplicationController::logAiAudit` stocke maintenant cet objectif comme `queryLabel` dans l'entrée `rememberedPatterns` (avec une liste d'objectifs génériques exclus pour ne pas figer un faux rôle, et conservation du label précédent si l'objectif de cette confirmation est générique). Exposé par `getRememberedPatterns()`, affiché en premier dans `SettingsView.vue`. Voir `docs/STRATEGY_ROOM.md` et `docs/PHASE_TRACKER.md` PHASE 23.

4. ~~**Rapport IA relié à la timeline Investigation**~~ **Fait le 19/08/2026** : pas de nouvelle donnée backend, uniquement une corrélation frontend. `InvestigationView.vue` charge maintenant le rapport IA automatiquement à l'ouverture si une enquête est active (au lieu d'attendre un clic manuel sur "Rapport IA"), et chaque étape/checkpoint de la timeline affiche une ligne "Pourquoi : ..." quand un `strategyScore` ou `telemetryInsight` du rapport correspond à son outil/kind (`stepStrategyReason()`/`checkpointStrategyReason()`, corrélation par mots-clés, ex. `tool` contenant `"aob"` → `aob_multimatch_guard`/`aob_quality_guard`).

5. ~~**Filtre de région par nom de module** dans `MemoryView.vue`~~ **Fait le 19/08/2026** : pas de champ module sur `killcore::MemoryRegion` côté backend, résolution côté frontend via `store.processModules` déjà peuplé à l'attache (`regionModuleName()`, nouvelle colonne + input de filtre). Voir `docs/PHASE_TRACKER.md` PHASE 23.

6. ~~**Historique d'écritures persistant/replay inter-session**~~ **Fait le 19/08/2026** : nouvelle séquence JSON `QSettings` (`writeHistory/process/<gameKey>/sequence`, ordre + doublons conservés, contrairement à `rememberedPatterns`) alimentée au même choke point que `registerWriteWatch` dans `writeMemoryValueConfirmed`. Nouveaux `getWriteHistorySequence`/`replayWriteHistorySequence`/`clearWriteHistorySequence`, UI dans `SettingsView.vue`. Voir `docs/PHASE_TRACKER.md` PHASE 23.

7. ~~**Écriture multi-adresses simultanée/atomique**~~ **Fait le 19/08/2026** : nouveau `core/process/process_suspend.h/.cpp` (`ProcessThreadsSuspendGuard`, RAII, suspend toutes les threads de la cible pendant l'écriture, reprend au destructeur) + `Q_INVOKABLE ApplicationController::writeMemoryValuesAtomic(targets, options)`. Validé en conditions réelles : écriture simultanée sur 2 adresses confirmée (`suspendedThreadCount`, relecture des deux adresses). Piège trouvé en testant : un auto-attach (KillEngine sur lui-même) provoquait un deadlock, le thread appelant se suspendant lui-même — corrigé en excluant `GetCurrentThreadId()` de la suspension. Exposé dans le panneau Candidats d'Expert (bouton "Écrire ensemble (atomique)" + `InfoDot` explicatif) en plus du connecteur d'automatisation. Voir `docs/PHASE_TRACKER.md` PHASE 26.

8. **Inspection d'objets managés .NET/CLR (ClrMD/SOS)** — **ClrMD MVP — fonctionnel et validé / intégration UI et enrichissements différés** (voir blocs "MVP livré" et "Consolidation" plus bas). Candidat identifié le 20/08/2026 lors de la reprise de l'investigation XP sur Solitaire (app UWP/.NET, voir `docs/STRATEGY_ROOM.md`). **Problème concret rencontré** : `analyzeStructureMemory` a bien confirmé qu'un champ "score de manche" et l'ancien champ "XP" sont voisins (offsets 0x40/0x48) dans le même objet, avec un layout cohérent avec un objet CLR géré (pointeur MethodTable à l'offset 0, auto-référence à 0x18) — mais KillEngine n'a aucun moyen de décoder ce layout *correctement* (noms de champs réels, type de l'objet, table de méthodes résolue en nom lisible) : tout ce qu'on peut faire aujourd'hui est deviner des offsets par tâtonnement sur des octets bruts. Un scan de pointeurs classique échoue aussi structurellement sur ce genre de cible (tas managé déplacé par le GC, confirmé par un scan de ~200M pointeurs sans aucun résultat). **Piste technique** : intégrer `Microsoft.Diagnostics.Runtime` (ClrMD, bibliothèque .NET officiellement supportée pour l'inspection de dumps/process managés) ou piloter `windbg`/`cdb` + l'extension SOS (`.loadby sos clr`, `!DumpObj`, `!DumpHeap`, `!GCRoot` — cette dernière commande en particulier résoudrait directement le problème "trouver un chemin stable vers un objet du tas managé" qui a fait échouer le scan de pointeurs classique). Nécessiterait soit un nouveau module `core/dotnet/` avec une dépendance native vers ClrMD (interop .NET/C++, coût d'intégration réel), soit un processus externe `cdb.exe`/`windbg` piloté et son output parsé (plus simple à intégrer mais dépend d'un outil externe pas forcément installé — aucun des deux n'était disponible sur la machine de dev lors de cette investigation). **Effort estimé** : élevé (nouvelle famille d'API/interop, pas une extension d'un module existant) — à ne pas sous-estimer avant de s'engager dessus. **Impact** : débloquerait potentiellement toute la classe de jeux/apps .NET/UWP modernes (déjà rencontrée deux fois : Solitaire ici, et documentée comme hypothèse dès la toute première session Solitaire du 19/08/2026 — "cohérent avec une UI XAML/managée").
   **Vérifié le 20/08/2026, avant de coder quoi que ce soit (voir `docs/STRATEGY_ROOM.md` entrée "Vérification candidat #8...")** : l'hypothèse "tas .NET managé" ci-dessus est **fausse pour Solitaire précisément**. Inspection statique du package installé (`AppxManifest.xml`, parsing manuel de l'en-tête PE) confirme que `Solitaire.exe` et `Microsoft.MicrosoftSolitaireCollection.dll` n'ont **aucun COR20/CLR header** (`ClrHeaderSize = 0`), aucun runtime .NET (`mrt100_app.dll`, `coreclr.dll`, `hostfxr.dll`) n'est présent dans le package, et `EntryPoint="Solitaire.App"` s'active par nom de classe WinRT — cohérent avec un exécutable **C++/WinRT natif**, pas C#/.NET. Le layout mémoire observé (vtable offset 0, auto-référence 0x18) est donc plus probablement un objet C++/COM natif qu'un objet CLR. **Conséquence : ClrMD/SOS ne débloquerait pas Solitaire (aucun CLR à quoi s'attacher)** — le candidat reste pertinent uniquement comme capacité générale pour de vraies cibles Desktop .NET Framework/CoreCLR (pas Mono/IL2CPP, pas UWP .NET Native), un périmètre plus étroit et moins prioritaire que ce que cette entrée supposait à l'origine. Nouvelle direction pour la suite de l'investigation XP Solitaire : résolution de vtable/RTTI natif, ou scan de pointeurs à profondeur/fenêtre élargie, ou remontée vers la fonction qui *calcule* la valeur plutôt que celle qui l'écrit — détail dans `docs/STRATEGY_ROOM.md`.
   **Cible de test dédiée livrée le 20/08/2026** : `tests/clr_targets/KillEngineClrTestTarget` (projet .NET séparé, build via `scripts/build-clr-test-target.ps1`) — un vrai process CoreCLR reproductible, indépendant de Solitaire ou de tout logiciel tiers, sur lequel le développement du module ClrMD/SOS a pu être déclenché et validé. Cahier des charges : `docs/KILLENGINE_CLR_TEST_TARGET_SPEC.md`.
   **MVP ClrMD livré le 20/08/2026** (demande explicite : "je préfère profiter de cette base pour aller jusqu'à un MVP ClrMD fonctionnel plutôt que laisser le chantier en attente") : `tools/clr_inspector/KillEngineClrInspector` — helper .NET dédié (`Microsoft.Diagnostics.Runtime` 4.0.732401), attache un CLR par PID (`DataTarget.AttachToProcess(pid, suspend:false)`), énumère le heap, lit champs primitifs/références/cycles, énumère les GC roots, expose tout via un pipe JSON-RPC (`KillEngineClrInspectorPipe`, protocole identique à `automation_pipe_server.h` — même client PowerShell réutilisable sans modification). Validé en conditions réelles sur `KillEngineClrTestTarget` : objet `Player` retrouvé, champs lus correctement, root `StrongHandle` retrouvé, et surtout — **après un `forceGC` Gen2 compactant réel (objet effectivement déplacé, adresse changée), le même objet logique est retrouvé** (valeur sentinelle + identité stable côté cible toutes deux confirmées identiques avant/après).
   **Consolidation le 20/08/2026** (demande explicite avant tout commit — "je veux simplement consolider la qualité du module avec quelques tests de non-régression supplémentaires") : suite d'auto-tests bout-en-bout étendue de 4 à **9/9 verts** (`tools/clr_inspector/KillEngineClrInspector.Tests`) — cycles GC successifs multiples avec identité cohérente à chaque cycle, plusieurs objets du même type distingués correctement, un objet réellement rendu inatteignable puis collecté (nouveau `DisposableProbe` dans la cible de test) vérifié **dans le même test** qu'un objet qui survit — pour prouver la distinction "déplacé" vs "disparu" plutôt que la supposer, terminaison brutale du process cible pendant une session active (l'inspecteur reste vivant, répond proprement en erreur puis redevient utilisable), et redémarrage avec un nouveau PID sans résidu de la session précédente. Noms de pipe rendus paramétrables (`KILLENGINE_CLR_TEST_TARGET_PIPE_NAME`/`KILLENGINE_CLR_INSPECTOR_PIPE_NAME`) pour permettre ces scénarios sans collision avec l'instance par défaut. **Limitation `StaticVar` gardée explicitement ouverte, non bloquante** (consigne explicite de l'utilisateur — ne pas la fermer prématurément). Détail complet : `docs/KILLENGINE_CLR_INSPECTOR_SPEC.md`.
   **Ce qui reste hors scope de ce MVP, volontairement (intégration UI et enrichissements différés)** : panneau UI KillEngine, déballage profond des collections (`List`/`Dictionary`/tableaux au-delà de leur propre référence), écriture/mutation via ClrMD (lecture seule pour l'instant), intégration `ApplicationController`/pipe d'automatisation principal, résolution du root `StaticVar` en attache passive live, publish self-contained réel.

**Explicitement hors scope, pas des candidats opérationnels immédiats** : DMA (section F, niveau 4 — hardware hors de portée d'un projet open par nature), contournement d'anti-cheat ou triche multijoueur en ligne (décliné explicitement le 16/08/2026 dans `docs/ULTIMATE_PRODUCT_GUIDELINE.md`, même raisonnement que pour Page Guard : l'angle légitime reste l'outillage QA/sécurité sur son propre build). Le driver noyau avec primitives mémoire sensibles, initialement classé "hors scope" ici, **a depuis été livré** : `tools/kernel_driver/KillEngineKernel/` contient le projet WDK `KillEngineKernel.sys` (build/signature/installation via `scripts/build-kernel-driver.ps1`/`scripts/install-kernel-driver.ps1`), `core/kernel/kernel_driver_bridge.*` détecte le device `\\.\KillEngineKernel` et expose lecture/écriture réelles (`readMemoryKernel`/`writeMemoryKernel`/`writeMemoryValueKernel`, pas seulement `probeKernelDriver`), câblées le 20/08/2026 (PHASE 39/42) au mode Expert (bouton d'escalade dans Candidats et écritures) et à l'Assistant (déclenchement direct sur demande explicite en chat), toujours gardées par `confirmRiskAction('injection', ...)`.

---

## Résumé visuel : où est KillEngine aujourd'hui vs les pros

```
Niveau de puissance
▲
│  Kernel driver / DMA          ← WeMod pro /payant
│  ─────────────────────────
│  DLL injection + hooks        ← Cheat Engine avancé     [Phase 20]
│  Auto-assembler scripting     ← Cheat Engine             [Phase 20]
│  ─────────────────────────
│  Freeze par breakpoint        ← Cheat Engine standard    [Phase 19]
│  Page guards / anti-debug     ← Trainers intermédiaires  [Phase 19]
│  Structure analyzer           ← Cheat Engine             [Phase 19]
│  ─────────────────────────
│  AOB scan + patch + trainer   ✅ FAIT (Phase 18)
│  Hardware breakpoints capture ✅ FAIT (Phase 17)
│  Pointer chains               ✅ FAIT (Phase 14)
│  AI tool-calling              ✅ FAIT (Phase 9)
│  Multi-type + unknown scan    ✅ FAIT (Phase 7)
│  Freeze polling + VirtualProt ✅ FAIT (Phase 16)
│  ─────────────────────────
│  Lecture/écriture RAM simple  ← Débutants
│
└────────────────────────────────────────────────► Temps
```

**KillEngine est déjà au niveau "Cheat Engine standard" pour le scan et l'AOB.
Le prochain saut est l'interception active (breakpoint freeze) puis l'injection.**
