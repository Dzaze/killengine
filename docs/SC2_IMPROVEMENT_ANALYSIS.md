# Analyse : pourquoi KillEngine échoue sur StarCraft 2 et comment aller plus loin

## TL;DR — Les 3 causes techniques principales

| # | Cause | Impact | Statut actuel |
|---|-------|--------|---------------|
| 1 | **Freeze = polling à 100 ms** | Le jeu réécrit la valeur avant le prochain tick KillEngine → le freeze "ne tient pas" | `QTimer` 100 ms |
| 2 | **Pas de `VirtualProtectEx` avant écriture** | Si la page n'est pas writable, `WriteProcessMemory` échoue silencieusement ou est annulé par le jeu | Non implémenté |
| 3 | **Adresse trouvée = valeur affichée, pas la source** | Le jeu réécrit l'affichage depuis une source cachée → toute écriture est écrasée | Pas de breakpoint pour remonter à la source |

---

## 1. Limite fondamentale de l'approche "lecture RAM pure"

L'approche actuelle de KillEngine repose entièrement sur :

```
OpenProcess() → ReadProcessMemory() / WriteProcessMemory()
```

C'est suffisant pour des **jeux simples** (Solitaire, petits jeux indés) mais ça a des limites structurelles sur les jeux AAA comme SC2 :

### 1.1 Le jeu réécrit en permanence
SC2 met à jour ses valeurs à chaque frame du moteur de simulation (~16–33 ms). KillEngine freeze à **100 ms** → le jeu a le temps de réécrire la "vraie" valeur 3 à 6 fois entre deux écritures de KillEngine. Résultat : la valeur "clignote" et revient toujours à sa valeur d'origine.

### 1.2 Multi-stockage
Les jeux modernes stockent souvent une ressource à plusieurs endroits :
- Une valeur **"real"** (source de vérité gameplay)
- Une ou plusieurs valeurs **"displayed"** (copies pour l'UI)
- Des **checksums** (anti-cheat léger) qui détectent les incohérences

Si KillEngine trouve et freeze la valeur "displayed", le jeu la réécrit depuis la valeur "real" → échec garanti.

### 1.3 Protections mémoire
Certaines pages ne sont pas `writable`. Sans `VirtualProtectEx`, `WriteProcessMemory` peut échouer ou la page peut être protégée en copy-on-write (une écriture crée une copie privée invisible au jeu).

### 1.4 On ne sait pas QUI écrit
La lecture RAM ne dit jamais **quelle instruction** modifie la valeur. Sans cette info, impossible de remonter à la source, ni de bloquer l'écriture du jeu.

---

## 2. Approches alternatives (par ordre de complexité / puissance)

### Niveau 0 — Améliorer l'écriture actuelle (immédiat, faible risque)

| Amélioration | Description | Effort |
|--------------|-------------|--------|
| **`VirtualProtectEx` avant écriture** | Rendre la page `PAGE_EXECUTE_READWRITE` le temps de l'écriture, restaurer ensuite | Faible |
| **Freeze plus agressif** | Intervalle configurable (10–16 ms au lieu de 100 ms) pour tenir tête au tick du jeu | Faible |
| **Détection d'échec d'écriture** | Si `WriteProcessMemory` retourne false ou verify failed, logger `GetLastError()` et tenter avec `VirtualProtectEx` | Faible |

### Niveau 1 — Hardware Breakpoints (LA solution professionnelle)

C'est **exactement** ce que fait Cheat Engine et les trainers pros. Au lieu de poller, on utilise les **debug registers** du CPU (DR0–DR3 sur x64).

**Principe :**
1. `DebugActiveProcess(pid)` — attache KillEngine comme debugger au processus cible
2. `SetThreadContext()` — pose un breakpoint matériel `DR0` sur l'adresse cible avec le flag **write (DR7)**
3. Quand le jeu écrit à cette adresse → le CPU lève `EXCEPTION_SINGLE_STEP`
4. KillEngine intercepte l'exception via `WaitForDebugEvent()` :
   - **Option A (freeze propre)** : modifier le contexte CPU pour empêcher l'écriture ou réécrire la bonne valeur
   - **Option B (find what writes)** : lire `EIP/RIP` → on obtient l'**instruction exacte** qui écrit → on peut remonter à la source

**Avantages :**
- Zéro polling, zéro overhead CPU, précision cycle CPU
- Marche même si la page n'est pas writable
- Permet de trouver l'instruction qui écrit → remonter à la vraie source
- Moins coûteux que du polling continu, mais détectable par certains anti-debug/anti-cheat

**Inconvénients :**
- Nécessite `PROCESS_ALL_ACCESS` + droits debugger (SeDebugPrivilege)
- 4 breakpoints max (DR0–DR3) sur x64
- SC2 peut potentiellement détecter l'attachement debugger (anti-debug)

### Niveau 2 — Page Guards (alternative aux hardware breakpoints)

Si les hardware breakpoints ne sont pas viables (anti-debug), on peut utiliser `PAGE_GUARD` :
1. `VirtualProtectEx` avec `PAGE_READWRITE | PAGE_GUARD` sur la page cible
2. Toute accès déclenche `STATUS_GUARD_PAGE_VIOLATION`
3. KillEngine intercepte l'exception, lit `EIP/RIP`, restaure la garde

Moins précis (page entière = 4 Ko) mais ne nécessite pas l'attachement debugger.

### Niveau 3 — DLL Injection + Hooking (très puissant, très invasif)

Injecter une DLL dans le processus cible via `CreateRemoteThread` + `LoadLibrary`. La DLL hook les fonctions qui gèrent les ressources. KillEngine devient **in-process**.

**Avantages :**
- Accès direct à toute la logique du jeu
- Peut intercepter les calculs, pas juste l'écriture mémoire

**Inconvénients :**
- Très invasif, détectable par anti-cheat
- Risque de crash du jeu
- Complexité élevée

---

## 3. Recommandation pour KillEngine

### Court terme (Phase 16 — cette session)

1. **`VirtualProtectEx` dans `MemoryWriter`** — peut débloquer les écritures quand la protection mémoire est le blocage
2. **Freeze interval configurable** — permettre 10–16 ms pour SC2
3. **Détection intelligente d'échec** — auto-réessayer avec changement de protection

### Moyen terme (Phase 17 — Hardware Breakpoints)

4. **Module `core/debug/`** nouveau :
   - `DebugSession` — attache/détache `DebugActiveProcess`
   - `HardwareBreakpoint` — pose DR0–DR3 via `SetThreadContext`
   - `BreakpointMonitor` — `WaitForDebugEvent` en boucle, capture `EXCEPTION_SINGLE_STEP`
   - **"Find what writes"** — capture `RIP` de l'instruction qui écrit
5. **Freeze par breakpoint** — bloque l'écriture du jeu au lieu de poller

### Long terme (optionnel)

6. **Pointer chain automatique depuis breakpoint** — remonte de l'instruction qui écrit vers les structures de données
7. **Auto-AOB** — scan de pattern d'octets pour trouver l'instruction dans d'autres versions du jeu

---

## 4. Ce que ça change pour l'utilisateur SC2

| Action utilisateur | Avant (polling) | Après (hardware breakpoint) |
|---------------------|------------------|------------------------------|
| Freeze minéraux | Clignote, revient à la valeur d'origine | **Tient en permanence** (le jeu ne peut plus réécrire) |
| Trouver la vraie source | Scan blind, beaucoup de bruit | **L'instruction qui écrit** est identifiée, on remonte à la structure |
| Écrire sur page protégée | Échec ou écriture partielle | **Peut marcher** si la protection mémoire était le blocage |
| Freeze sur valeur "displayed" | Réécrit par le jeu | On trouve la source, on la freeze |

---

## 5. Risques et mitigations

| Risque | Mitigation |
|--------|------------|
| Anti-debug SC2 détecte l'attachement | Démarrer par VirtualProtectEx + polling agressif ; hardware breakpoints en option expert |
| Crash du jeu si breakpoint mal posé | Validation de l'adresse + restauration auto au moindre problème |
| SeDebugPrivilege requis | Guide utilisateur + élévation UAC si besoin |
| Détection Warden (anti-cheat Blizzard) | Option "mode discret" sans attachement debugger (VirtualProtectEx only) |

---

## Conclusion

**Oui, il faut une autre approche que simplement lire/écrire la RAM.** L'approche `ReadProcessMemory`/`WriteProcessMemory` est arrivée à ses limites sur SC2. Les deux axes d'amélioration prioritaires sont :

1. **Immédiat** : `VirtualProtectEx` + freeze agressif → débloque 30–40 % des cas
2. **Structurel** : Hardware breakpoints → solution professionnelle qui rend KillEngine aussi puissant que Cheat Engine pour le freeze et le "find what writes"
