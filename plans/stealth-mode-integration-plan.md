# Plan d'intégration : Stealth Mode via Pipe d'automatisation

## Objectif
Créer un wrapper unifié `applyStealthMode(profile)` qui active toutes les protections anti-détection en une seule commande, accessible via le pipe d'automatisation avec un mot-clé générique (ex: `"sc2"`).

## Architecture

### 1. Wrapper unifié dans `ApplicationController`

```cpp
// apps/desktop/application_controller.h
Q_INVOKABLE QVariantMap applyStealthMode(const QString& profile);
Q_INVOKABLE QVariantMap restoreStealthMode();
Q_INVOKABLE QVariantMap getStealthModeStatus() const;
```

**Profils supportés :**
- `"sc2"` : active tout (anti-debug + process mask + DLL mask)
- `"default"` : active uniquement l'anti-debug
- `"minimal"` : active uniquement le masquage de processus

**Comportement :**
- `applyStealthMode("sc2")` → active `AntiDebugSession`, `ProcessMask`, `DllMask`
- `restoreStealthMode()` → désactive tout et restaure les noms originaux
- `getStealthModeStatus()` → retourne l'état actuel (quels modules sont actifs)

### 2. Interface TypeScript dans `backend.ts`

```typescript
export interface StealthModeResult {
  success: boolean
  profile: string
  modules: {
    antiDebug: boolean
    processMask: boolean
    dllMask: boolean
  }
  error?: string
}

export interface StealthModeStatus {
  active: boolean
  profile: string
  modules: {
    antiDebug: boolean
    processMask: boolean
    dllMask: boolean
  }
}
```

### 3. Pipe d'automatisation

Le pipe utilise la réflexion `QMetaMethod` — pas de modification nécessaire. L'appel sera :
```json
{"id":1,"method":"applyStealthMode","params":["sc2"]}
```

### 4. Fichiers à modifier

| Fichier | Action |
|---------|--------|
| `apps/desktop/application_controller.h` | Ajouter 3 méthodes Q_INVOKABLE |
| `apps/desktop/application_controller.cpp` | Implémenter les 3 méthodes |
| `ui/src/services/backend.ts` | Ajouter interfaces + méthodes dans BackendController + mock |
| `docs/PHASE_TRACKER.md` | Ajouter entrée PHASE 253 |

### 5. Flux d'utilisation

```
Script Lua / Pipe → applyStealthMode("sc2")
  → AntiDebugSession::start()
  → ProcessMask::maskCurrentProcess("svchost.exe")
  → DllMask::maskDll(pid, "KillEnginePageGuardHandler.dll")
  → Retourne {success: true, profile: "sc2", modules: {antiDebug: true, processMask: true, dllMask: true}}
```

### 6. Risques

- **Anti-debug** : les breakpoints INT3 peuvent être détectés par des anti-cheat avancés — à tester sur SC2
- **Process mask** : `NtSetInformationProcess` peut ne pas fonctionner sur tous les processus — fallback silencieux
- **DLL mask** : `EnumProcessModules` ne masque pas vraiment la DLL, juste la trouve — à améliorer avec PEB manipulation si nécessaire

### 7. Tests

- Build : `.\scripts\build.ps1`
- Tests unitaires : `.\build\bin\killengine_unit_tests.exe`
- Smoke test pipe : `.\scripts\automation-pipe-call.ps1 -Method applyStealthMode -ParamsJson '["sc2"]'`