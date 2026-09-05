# Plan : Phases 2-3-4 — Capacités réseau offensives

**Date** : 2026-09-05  
**Statut** : Proposition  
**Scope** : Processus attaché uniquement

---

## Phase 2 — Proxy HTTP (injection, risque élevé)

### Objectif
Intercepter et modifier les requêtes HTTP/HTTPS du jeu en temps réel via injection DLL + hook WinINet/WinHTTP.

### Architecture

```
KillEngine.exe
  └── ApplicationController::startHttpProxy()
        └── injectDll(KillEngineHttpProxy.dll)
              └── MinHook sur HttpSendRequest/WinHttpSendRequest
                    └── IPC shared memory ←→ KillEngine.exe
                          └── UI affiche les requêtes interceptées
                          └── UI modifie le body → IPC → DLL → requête modifiée envoyée
```

### Module C++ : `core/inject/http_proxy.*`

**`http_proxy.h`** :
```cpp
struct HttpProxyConfig {
    int port{8080};
    bool interceptHttps{false};
};

struct HttpProxyRequest {
    QString id;
    QString method;
    QString url;
    QString requestBody;
    QString responseBody;
    qint64 timestamp;
    bool modified{false};
};

class HttpProxySession {
public:
    bool start(const ProcessHandle& process, const HttpProxyConfig& config,
               const QString& handlerPath, QString* error);
    void stop();
    QVector<HttpProxyRequest> getRequests() const;
    bool modifyRequest(const QString& requestId, const QString& newBody);
private:
    // IPC shared memory + MinHook session
};
```

**`http_proxy.cpp`** :
- Injection de `KillEngineHttpProxy.dll` via `injectDll()`
- La DLL hook `HttpSendRequest` (WinINet) et `WinHttpSendRequest` (WinHTTP) via MinHook
- Les requêtes interceptées sont stockées dans un `IpcSharedMemory` (même pattern que `api_hook_ipc.h`)
- KillEngine.exe lit les requêtes via `getRequests()` et les affiche dans l'UI
- L'UI modifie le body → `modifyRequest()` → la DLL envoie la requête modifiée

### Backend : `ApplicationController`

```cpp
Q_INVOKABLE QVariantMap startHttpProxy(int port, bool interceptHttps);
Q_INVOKABLE QVariantMap stopHttpProxy();
Q_INVOKABLE QVariantMap getHttpProxyRequests();
Q_INVOKABLE QVariantMap modifyHttpRequest(const QString& requestId, const QString& newRequestBody);
```

### DLL Handler : `core/inject/http_proxy_handler.cpp` (nouveau)

- MinHook sur `HttpSendRequest` (wininet.dll) et `WinHttpSendRequest` (winhttp.dll)
- Capture : method, URL, headers, body
- Stockage dans shared memory IPC
- Si `modifyRequest` appelé : remplace le body et appelle l'original modifié
- Si `interceptHttps` : hook aussi `InternetConnect` + `HttpSendRequest` avec certificat root CA auto-signé (complexe, reporté à v2)

### Fichiers à créer/modifier
| Fichier | Action |
|---|---|
| `core/inject/http_proxy.h/.cpp` | **Nouveau** — session proxy HTTP |
| `core/inject/http_proxy_handler.cpp` | **Nouveau** — DLL handler injecté |
| `core/inject/CMakeLists.txt` | +2 fichiers |
| `apps/desktop/application_controller.h` | +4 méthodes (déjà déclarées en stub) |
| `apps/desktop/application_controller.cpp` | Implémenter les 4 méthodes |

---

## Phase 3 — Spoof DNS (UAC, risque moyen)

### Objectif
Rediriger un domaine vers une IP locale (ex: `127.0.0.1`) via le fichier `hosts` Windows.

### Architecture

```
KillEngine.exe
  └── ApplicationController::spoofDns(domain, targetIp)
        └── PowerShell elevated (UAC)
              └── Écrit dans C:\Windows\System32\drivers\etc\hosts
                    └── Ligne: <targetIp> <domain>
```

### Backend : `ApplicationController`

```cpp
Q_INVOKABLE QVariantMap spoofDns(const QString& domain, const QString& targetIp);
Q_INVOKABLE QVariantMap restoreDns(const QString& domain);
```

**Implémentation** :
- PowerShell elevated via `ShellExecuteExW` avec `runas` (même pattern que `blockProcessNetwork`)
- Lecture du fichier `hosts` actuel
- Ajout de la ligne `<targetIp> <domain>` si pas déjà présente
- Pour `restoreDns` : retirer la ligne correspondante
- Retourne `{success, error}`

### Fichiers à modifier
| Fichier | Action |
|---|---|
| `apps/desktop/application_controller.cpp` | Implémenter les 2 méthodes (déjà en stub) |

---

## Phase 4 — Lag switch (injection, risque élevé)

### Objectif
Retarder artificiellement les fonctions `recv`/`WSARecv` du process attaché de X ms.

### Architecture

```
KillEngine.exe
  └── ApplicationController::setLagSwitch(enabled, delayMs)
        └── injectDll(KillEngineLagSwitch.dll)
              └── MinHook sur recv/WSARecv (ws2_32.dll)
                    └── Hook: Sleep(delayMs) → appel original
```

### Module C++ : `core/inject/lag_switch.*`

**`lag_switch.h`** :
```cpp
class LagSwitchSession {
public:
    bool start(const ProcessHandle& process, int delayMs,
               const QString& handlerPath, QString* error);
    void stop();
    bool isActive() const;
    int delayMs() const;
private:
    // IPC shared memory + MinHook session
};
```

**`lag_switch.cpp`** :
- Injection de `KillEngineLagSwitch.dll` via `injectDll()`
- La DLL hook `recv` et `WSARecv` (ws2_32.dll) via MinHook
- Le hook appelle `Sleep(delayMs)` avant de passer à l'original
- `stop()` retire les hooks

### DLL Handler : `core/inject/lag_switch_handler.cpp` (nouveau)

- MinHook sur `recv` et `WSARecv` de `ws2_32.dll`
- Shared memory IPC pour lire `delayMs` et contrôler l'état
- Si `delayMs == 0` : passe directement à l'original (pas de délai)

### Backend : `ApplicationController`

```cpp
Q_INVOKABLE QVariantMap setLagSwitch(bool enabled, int delayMs);
```

### Fichiers à créer/modifier
| Fichier | Action |
|---|---|
| `core/inject/lag_switch.h/.cpp` | **Nouveau** — session lag switch |
| `core/inject/lag_switch_handler.cpp` | **Nouveau** — DLL handler injecté |
| `core/inject/CMakeLists.txt` | +2 fichiers |
| `apps/desktop/application_controller.h` | +1 méthode (déjà déclarée en stub) |
| `apps/desktop/application_controller.cpp` | Implémenter la méthode |

---

## Ordre d'implémentation recommandé

1. **Phase 3 (Spoof DNS)** — Le plus simple, juste PowerShell + fichier hosts, pas d'injection
2. **Phase 4 (Lag switch)** — Injection + MinHook, mais logique simple (Sleep)
3. **Phase 2 (Proxy HTTP)** — Le plus complexe, nécessite IPC bidirectionnel + gestion HTTPS

---

## Pièges connus

- **Proxy HTTPS** : injection de certificat root CA — complexe, nécessite de manipuler le store certificat du process. Reporté à v2.
- **Lag switch** : hook MinHook sur `recv`/`WSARecv` — peut crasher si le hook est mal installé, tester sur `KillEngineTestTarget.exe` d'abord.
- **Spoof DNS** : écriture dans `C:\Windows\System32\drivers\etc\hosts` — UAC requis, fichier en lecture seule par défaut sur certaines configs.
- **Tous les handlers injectés** : la DLL reste chargée dans la cible jusqu'à la fin du processus — une réinjection sur le même PID n'est pas supportée (DllMain ne se rejoue pas).
