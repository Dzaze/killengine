# KillEngine — Feuille de route pour rendre le hacking plus puissant

> Ce document part de l'état actuel (Phase 18 partielle) et identifie les **axes concrèts** pour aller plus loin.
> Chaque proposition est ancrée dans le code existant et précise les modules/fichiers à étendre.

---

## État des lieux en une ligne

KillEngine est aujourd'hui un **Cheat Engine "lite"** très avancé côté scan (multi-type, unknown, UI string, pointer chains, freeze) avec un début de couche pro (hardware breakpoints, AOB/trainer, AI tool-calling). Le gap principal se situe sur **l'interception active** (bloquer l'écriture du jeu) et **l'injection in-process**.

---

## A. Freeze par hardware breakpoint (LE plus impactant)

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

**Problème :** Les patchs actuels sont des bytes fixes. Les vrais trainers utilisent des scripts avec allocation mémoire, labels, conditions.

**Ce qui existe :** `core/patch/instruction_patch_suggester.cpp` propose des templates NOP/INT3/RET.

**Ce qu'il faut ajouter :** `core/scripting/auto_assembler.h/.cpp`
1. **Mini-langage type Cheat Engine** :
   ```
   alloc(newmem, 256)
   label(returnhere)
   label(exit)

   newmem:
     mov [rax+08], (int)9999   // force HP
     jmp exit

   "SC2.exe"+0x12345:
     jmp newmem
     nop
   exit:
   ```
2. **Assembleur runtime** : intégrer un moteur comme [keystone](https://www.keystone-engine.org/) pour compiler les mnemonics en bytes.
3. **Exécution** : allouer (`VirtualAllocEx`), écrire le code compilé, poser le `jmp` à l'adresse cible, stocker les bytes originaux pour restore.
4. **Sauvegarde** : le script devient un type de patch dans `ProfileStore`.

**Effort :** Élevé. **Impact :** Permet les cheat complexes (infinite HP, one-hit kill, no clip).

---

## F. Anti-anti-cheat / Mode stealth

**Problème :** Les jeux détectent `DebugActiveProcess` (Warden Blizzard, EAC, BattlEye). Le hardware breakpoint devient inutilisable.

**Solutions en cascade (du moins au plus invasif) :**

| Niveau | Technique | Détection | Implémentation |
|--------|-----------|-----------|----------------|
| 0 | Polling + VirtualProtectEx | Aucune | ✅ Déjà fait |
| 1 | Page Guards (`PAGE_GUARD`) | Faible | `core/debug/page_guard.*` — nouveau |
| 2 | Hardware BP via `NtSetInformationThread` au lieu de `DebugActiveProcess` | Moyenne | Extension `hardware_breakpoint.cpp` |
| 3 | Kernel driver (`\\.\KillEngine`) | Forte (si signé) | Hors scope V2 |
| 4 | DMA hardware (carte PCIe) | Indétectable | Hors scope |

**Ajout concret Phase 19 : `core/debug/page_guard.h/.cpp`**
- `VirtualProtectEx` avec `PAGE_READWRITE | PAGE_GUARD` sur la page cible.
- Intercepter `STATUS_GUARD_PAGE_VIOLATION` via `AddVectoredExceptionHandler`.
- Point critique : pour un processus cible externe, le VEH doit vivre **dans le processus cible** (DLL/shellcode injecté). Un VEH installé dans KillEngine ne reçoit pas les exceptions du jeu.
- Lire `RIP` depuis `EXCEPTION_POINTERS`.
- Restaurer la garde après passage.
- **Avantage clé :** pas d'attachement debugger → indétectable par `IsDebuggerPresent`.

**Effort :** Moyen. **Impact :** Débloque les jeux avec anti-debug léger.

---

## G. Hotkeys globales + Trainer overlay

**Problème :** Pour activer/désactiver un freeze/patch, l'utilisateur doit alt-tab vers KillEngine.

**Solution :**

1. **Hotkeys globaux** (`core/input/global_hotkey.h/.cpp`) :
   - `RegisterHotKey` avec modificateurs (Ctrl+F1, etc.).
   - Associer chaque hotkey à une action profil : toggle freeze, toggle patch, write value.
   - Marche même quand KillEngine est en arrière-plan.

2. **Overlay in-game** (`apps/desktop/overlay/`) :
   - Fenêtre transparente `Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool`.
   - `SetWindowLongPtr` avec `WS_EX_LAYERED | WS_EX_TRANSPARENT` pour laisser les clics passer.
   - Afficher : états des freezes, valeurs live, notifications de patch appliqué.
   - Toggle avec une hotkey globale.

**Effort :** Moyen. **Impact :** UX de trainer pro, utilisable en plein écran.

---

## H. AI proactive + Auto-stratégie

**État réel au 16/08/2026, mis à jour après le nettoyage du même jour :** le workflow décrit plus bas dans une version précédente de ce document existe maintenant réellement, avec une seule implémentation par bout de logique.

- `ApplicationController::startAutoResolve` (`apps/desktop/application_controller.cpp`, ~350 lignes) reste le moteur de la phase "trouver la valeur" : scan exact multi-type → fallback scan chiffré XOR borné → fallback Trace UI string borné → capture Unknown bornée, avec `executedSafeSteps` audité et confirmation obligatoire avant tout write/freeze.
- **Chaînage réel écrit → find-what-writes → AOB → patch** : dès qu'une capture Find What Writes réussit (bouton `Écrit par` dans Expert), la signature AOB et les suggestions de patch se génèrent automatiquement (lecture seule, `autoChainFindWhatWritesResult` dans `ExpertView.vue`, réutilise le même chemin que le clic manuel "Analyser"). Sauvegarder en Trainer ou appliquer un patch restent des clics explicites — jamais automatiques.
- **Persistance après écriture** : `suggestStableLocatorForAddress` cherche une chaîne de pointeurs stable après une écriture confirmée ; le store frontend le déclenche automatiquement en fond (silencieux si rien trouvé, dédupliqué par adresse) après chaque écriture Expert réussie.
- **Nettoyage du doublon `AutoResolver`/`summarizeAutoTelemetry`** : les deux implémentations mortes ou dupliquées ont été retirées. `killai::computeAutoResolveTelemetryReport` est maintenant la seule fonction qui analyse la télémétrie et produit les insights actionnables ; `getAutoResolveReport` l'appelle directement au lieu de la réimplémenter en ligne. Testée dans `tests/unit/test_auto_resolver.cpp`, y compris le cas "500 candidats après Analyser sources" qui n'avait aucune couverture avant.
- L'apprentissage par jeu existe déjà, partiellement : `rememberAutoResolverProgress` persiste des compteurs par processus (`autoResolver/process/<jeu>` dans `QSettings`) — démarrages, réductions, checkpoints bas-candidats. Pas encore de vraie mémoire de pattern structuré ("minéraux = Int32 à +0x1A4") comme décrit plus bas.

**Ce qui reste à faire, dans le prolongement de ce qui existe :**

1. **Déclencher le chaînage automatiquement, pas seulement au clic "Écrit par".** Rien aujourd'hui ne détecte "l'écriture confirmée ne tient pas" pour lancer Find What Writes tout seul — ça demande soit une vérification/polling après write (pas encore instrumentée), soit une intention en langage naturel ("ça ne tient pas") côté `ai_engine.cpp`/`tool_registry.cpp` (n'existe pas encore). Le chaînage à partir du clic existe déjà et est réel ; c'est le déclenchement en amont qui manque.
2. **Vraie mémoire de pattern par jeu.** Remplacer/étendre les compteurs `QSettings` actuels par une structure explicite (module, offset relatif, type, rôle probable — "argent", "vie"...) réutilisable au scan suivant sur le même exécutable, au lieu de repartir d'un scan exact à froid à chaque session.
3. **Rapport IA plus explicatif.** `getAutoResolveReport` existe et fonctionne (synthèse des échecs exact/unknown/AOB). Le relier plus directement à la timeline Investigation pour que chaque checkpoint explique *pourquoi* cette action a été choisie, pas seulement *quoi*.

**Hors scope, volontairement** : contournement d'anti-cheat, triche en partie multijoueur en ligne. Demandé explicitement le 16/08/2026, décliné pour la même raison que documentée dans `docs/ULTIMATE_PRODUCT_GUIDELINE.md` — construire ou documenter comment le construire revient au même risque, indépendamment de qui écrit le code ensuite. L'angle légitime et commercialement valable reste l'outillage QA/sécurité pour des développeurs testant leur propre build en environnement contrôlé, qui n'a besoin d'aucun contournement puisque c'est leur propre logiciel.

**Effort :** Fait pour la partie chaînage aval (find-what-writes → patch) et le nettoyage de duplication. Reste moyen pour le déclenchement amont. **Impact :** Différenciateur majeur vs Cheat Engine.

---

## I. Améliorations transverses "quick wins"

| Amélioration | Module | Effort | Impact |
|--------------|--------|--------|--------|
| **Scan groupé** (plusieurs valeurs proches) | `core/scanner/group_scan.*` | Moyen | Trouve les structures rapidement |
| **Scan différenciel rapide** (snapshot A vs B, XOR des pages) | `core/snapshot/snapshot_store.cpp` | Faible | Unknown scan 10× plus rapide |
| **Dump mémoire → fichier** (.dmp par région) | `core/memory/memory_reader.cpp` | Faible | Debug/expertise offline |
| **Recherche de chaînes de pointeurs depuis breakpoint** | `core/pointer/pointer_scanner.cpp` | Faible | Remonte à la base automatiquement |
| **Watch expressions** (adresse + offset + formule) | Nouveau | Moyen | Suit les pointeurs en live |
| **Édition hex inline** dans l'inspecteur mémoire | `ui/src/views/ExpertView.vue` | Faible | UX power-user |
| **Filtres de région avancés** (par module, par commit charge) | `core/memory/memory_map.cpp` | Faible | Moins de bruit |
| **Historique d'écritures avec replay** | `core/profiles/profile_store.cpp` | Faible | Audit/debugging |

---

## Priorisation recommandée (impact × faisabilité)

### Phase 19 — Breakpoint freeze + Structure analyzer (gros gain, code existant)
1. **Freeze par hardware breakpoint** (A) — tient enfin sur SC2
2. **Analyseur de structures** (D) — déduit les layouts automatiquement
3. **Scan de valeurs chiffrées** (C) — débloque les jeux obfusqués
4. **Page Guards stealth** (F) — alternative anti-debug

### Phase 20 — Injection & Scripting (passe au niveau pro)
5. **Injection DLL + hooks** (B) — freeze invincible
6. **Auto-assembler** (E) — scripts complexes
7. **Hotkeys + overlay** (G) — UX trainer

### Phase 21 — AI proactive
8. **Auto-résolution workflow** (H) — l'IA enchaîne seule
9. **Apprentissage par jeu** — mémoire des patterns

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
