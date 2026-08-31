# Plan d'amélioration anti-détection SC2 — Mise à jour 30/08/2026

## Contexte

L'utilisateur veut rendre KillEngine moins détectable par StarCraft 2 (SC2) pour éviter les crashes. SC2 est un jeu local/offline (pas de protection anti-cheat en ligne). Le problème est que SC2 détecte KillEngine et crash.

## État réel du code (30/08/2026)

### ✅ Déjà implémenté

| Amélioration | Fichier | Statut |
|---|---|---|
| VirtualProtectEx avant écriture | [`core/memory/memory_writer.cpp`](core/memory/memory_writer.cpp:21) — `makeWritable()` + fallback dans `write()` | ✅ Complet |
| Freeze interval configurable (10-2000ms) | [`apps/desktop/freeze_hotkey_overlay_manager.cpp`](apps/desktop/freeze_hotkey_overlay_manager.cpp:157) — `setFreezeInterval()` | ✅ Complet |
| Détection intelligente d'échec d'écriture | [`core/memory/memory_writer.cpp`](core/memory/memory_writer.cpp:127) — `GetLastError()` + retry VirtualProtectEx | ✅ Complet |
| Page Guard (PAGE_GUARD + VEH) | [`core/debug/page_guard.cpp`](core/debug/page_guard.cpp:19) — `PageGuardSession` | ✅ Complet |
| Hooking in-process (MinHook) | [`core/inject/api_hook.h`](core/inject/api_hook.h:53) — `ApiHookSession` | ✅ Complet |
| Injection DLL (CreateRemoteThread) | [`core/inject/dll_injector.cpp`](core/inject/dll_injector.cpp:124) — `injectDll()` | ✅ Complet |
| Injection shellcode | [`core/inject/dll_injector.h`](core/inject/dll_injector.h:53) — `injectShellcode()` | ✅ Complet |
| Inline hook (detour) | [`core/inject/function_hook.h`](core/inject/function_hook.h:47) — `installInlineHook()` | ✅ Complet |
| Freeze par hardware breakpoint | [`core/debug/breakpoint_freeze.cpp`](core/debug/breakpoint_freeze.cpp:223) — `BreakpointFreezeManager` | ✅ Complet |
| In-process breakpoint (VEH) | [`core/debug/inprocess_breakpoint.cpp`](core/debug/inprocess_breakpoint.cpp:278) | ✅ Complet |

### ❌ Non implémenté

| Amélioration | Priorité | Risque | Description |
|---|---|---|---|
| NtCreateThreadEx (alternative à CreateRemoteThread) | Haute | Faible | Utiliser `NtCreateThreadEx` au lieu de `CreateRemoteThread` pour éviter la détection d'injection |
| Masquage de processus | Moyenne | Faible | Masquer le nom du processus KillEngine pour éviter la détection par nom |
| Masquage de DLL | Moyenne | Faible | Masquer la DLL injectée pour éviter la détection |
| Anti-anti-debug | Haute | Moyen | Contourner les mécanismes anti-debug de SC2 (IsDebuggerPresent, CheckRemoteDebuggerPresent, NtQueryInformationProcess) |

## Analyse des mécanismes de détection SC2

SC2 utilise plusieurs mécanismes de détection :

1. **Anti-debug** : SC2 vérifie si un debugger est attaché via `IsDebuggerPresent`, `CheckRemoteDebuggerPresent`, ou `NtQueryInformationProcess`
2. **Détection d'injection** : SC2 détecte les DLL injectées via `CreateRemoteThread` + `LoadLibraryW`
3. **Détection d'accès mémoire** : SC2 peut détecter les accès mémoire suspects via `ReadProcessMemory`/`WriteProcessMemory`
4. **Détection de breakpoints** : SC2 peut détecter les hardware breakpoints via les registres DR0-DR3

## Plan d'action pour les améliorations manquantes

### Phase 1 : NtCreateThreadEx (alternative à CreateRemoteThread)

**Objectif** : Utiliser `NtCreateThreadEx` au lieu de `CreateRemoteThread` pour éviter la détection d'injection.

**Fichiers concernés** :
- `core/inject/dll_injector.cpp` : Ajouter une option `useNtCreateThreadEx` dans `InjectDllOptions`
- `core/inject/dll_injector.h` : Ajouter le champ `useNtCreateThreadEx` dans `InjectDllOptions`

**Implémentation** :
1. Ajouter `bool useNtCreateThreadEx{false}` dans `InjectDllOptions`
2. Dans `injectDll()`, si `options.useNtCreateThreadEx` est vrai, utiliser `NtCreateThreadEx` au lieu de `CreateRemoteThread`
3. `NtCreateThreadEx` est une fonction de `ntdll.dll` qui crée un thread distant sans passer par `CreateRemoteThread`
4. Ajouter un test unitaire pour vérifier que l'option fonctionne

**Risques** :
- `NtCreateThreadEx` est une fonction non documentée de Windows
- Peut ne pas fonctionner sur toutes les versions de Windows
- Nécessite de résoudre l'adresse de `NtCreateThreadEx` dans `ntdll.dll`

### Phase 2 : Masquage de processus

**Objectif** : Masquer le nom du processus KillEngine pour éviter la détection par nom.

**Fichiers concernés** :
- `apps/desktop/main.cpp` : Ajouter le masquage de processus au démarrage
- `apps/desktop/application_controller.cpp` : Ajouter une méthode `maskProcessName()`

**Implémentation** :
1. Utiliser `NtSetInformationProcess` avec `ProcessBasicInformation` pour changer le nom du processus
2. Ou utiliser `RtlInitUnicodeString` + `NtSetInformationProcess` pour changer le nom du processus
3. Ou utiliser une technique de "process hollowing" pour masquer le processus

**Risques** :
- Le masquage de processus peut être détecté par des techniques avancées
- Peut causer des problèmes de stabilité
- Nécessite des privilèges élevés

### Phase 3 : Masquage de DLL

**Objectif** : Masquer la DLL injectée pour éviter la détection.

**Fichiers concernés** :
- `core/inject/dll_injector.cpp` : Ajouter une option `maskDll` dans `InjectDllOptions`
- `core/inject/dll_injector.h` : Ajouter le champ `maskDll` dans `InjectDllOptions`

**Implémentation** :
1. Utiliser `NtSetInformationProcess` avec `ProcessModuleInformation` pour masquer la DLL
2. Ou utiliser une technique de "DLL hollowing" pour masquer la DLL
3. Ou utiliser une technique de "DLL proxying" pour masquer la DLL

**Risques** :
- Le masquage de DLL peut être détecté par des techniques avancées
- Peut causer des problèmes de stabilité
- Nécessite des privilèges élevés

### Phase 4 : Anti-anti-debug

**Objectif** : Contourner les mécanismes anti-debug de SC2.

**Fichiers concernés** :
- `core/debug/anti_debug.cpp` : Nouveau fichier pour les techniques anti-anti-debug
- `core/debug/anti_debug.h` : Header pour les techniques anti-anti-debug

**Implémentation** :
1. Hook `IsDebuggerPresent` pour retourner `FALSE`
2. Hook `CheckRemoteDebuggerPresent` pour retourner `FALSE`
3. Hook `NtQueryInformationProcess` pour retourner `FALSE` pour `ProcessDebugPort`
4. Hook `NtSetInformationProcess` pour empêcher la désactivation du debug
5. Utiliser `NtSetInformationProcess` avec `ProcessDebugPort` pour désactiver le debug

**Risques** :
- Les techniques anti-anti-debug peuvent être détectées par des techniques avancées
- Peut causer des problèmes de stabilité
- Nécessite des privilèges élevés

## Tests

- Tests unitaires pour chaque amélioration
- Tests d'intégration avec KillEngineTestTarget
- Tests live avec SC2 (local/offline)

## Risques

- **Anti-debug** : SC2 peut détecter l'attachement debugger
- **Crash du jeu** : Si un breakpoint est mal posé
- **SeDebugPrivilege requis** : Guide utilisateur + élévation UAC si besoin
- **Cible refuse tout attachement de débogueur** : Option sans canal de debug Win32

## Conclusion

Le plan d'amélioration anti-détection SC2 est divisé en 4 phases. Les améliorations déjà implémentées (VirtualProtectEx, freeze agressif, Page Guard, hooking in-process) couvrent une grande partie des besoins. Les améliorations manquantes (NtCreateThreadEx, masquage de processus, masquage de DLL, anti-anti-debug) peuvent rendre KillEngine complètement indétectable.