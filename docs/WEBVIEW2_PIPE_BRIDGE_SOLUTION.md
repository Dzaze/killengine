# Solution : Bridge CDP via Pipe Nommé pour WebView2

## Problème

Le port CDP 9333 est bloqué par l'AppContainer de Solitaire/SC2 via WFP (Windows Filtering Platform). Les connexions TCP entrantes sont dropées silencieusement.

## Solution Implémentée

### Architecture

```
┌─────────────────┐         ┌──────────────────┐         ┌─────────────────┐
│  KillEngine.exe │◄───────►│  Pipe Nommé      │◄───────►│  Helper DLL     │
│  (Qt LocalSocket)│  Local  │  (Windows IPC)   │  Local  │  (dans SC2)     │
└─────────────────┘         └──────────────────┘         └────────┬────────┘
                                                                   │
                                                                   │ WebSocket
                                                                   │ localhost:9333
                                                                   │ (interne)
                                                                   ▼
                                                          ┌─────────────────┐
                                                          │  WebView2 CDP   │
                                                          │  (dans SC2)     │
                                                          └─────────────────┘
```

### Composants

1. **CdpPipeBridge** (`core/webview2/cdp_pipe_bridge.h/.cpp`)
   - Côté KillEngine
   - Utilise `QLocalSocket` pour se connecter au pipe
   - API identique à CdpClient (drop-in replacement)

2. **CdpPipeHelper DLL** (`core/webview2/cdp_pipe_helper_dll.h`)
   - DLL injectée dans le processus cible
   - Se connecte au CDP via WebSocket (localhost:9333) depuis l'intérieur
   - Crée un pipe nommé accessible depuis l'extérieur
   - Relaie les messages dans les deux sens

### Pourquoi ça marche

- Le WebSocket vers localhost:9333 fonctionne depuis l'intérieur du processus (pas de filtre WFP sur les connexions sortantes)
- Le pipe nommé est un canal IPC local Windows, pas du TCP, donc pas filtré par AppContainer
- Le pipe est créé par le helper injecté, qui a les mêmes privilèges que le processus cible

### Fichiers Créés

| Fichier | Description |
|---------|-------------|
| `core/webview2/cdp_pipe_bridge.h` | Header du bridge côté KillEngine |
| `core/webview2/cdp_pipe_bridge.cpp` | Implémentation du bridge |
| `core/webview2/cdp_pipe_helper_dll.h` | Header de la DLL helper |

### Prochaines Étapes

1. **Implémenter la DLL helper** (`cdp_pipe_helper_dll.cpp`)
   - Utiliser WinHTTP pour le WebSocket client
   - Utiliser Windows API pour le pipe nommé
   - Gérer le framing WebSocket (opcode, mask, etc.)

2. **Intégrer l'injection**
   - Utiliser `core/inject/dll_injector.h` existant
   - Injecter la DLL au moment de l'attachement
   - Appeler `StartCdpPipeHelper` via le point d'entrée exporté

3. **Mettre à jour WebView2Inspector**
   - Ajouter une méthode `connectViaPipe(quint32 processId)`
   - Fallback automatique : TCP → Pipe → Erreur

4. **Tests**
   - Vérifier que le pipe est créé après injection
   - Vérifier que les commandes CDP passent
   - Tester avec Solitaire

### Avantages

- ✅ Contourne complètement le blocage AppContainer
- ✅ Pas besoin de modifier le processus cible (juste injecter une DLL)
- ✅ Architecture propre et réutilisable
- ✅ Peut fonctionner avec n'importe quel processus WebView2

### Risques

- ⚠️ Nécessite l'injection DLL (peut être détecté par anti-cheat)
- ⚠️ La DLL helper doit être compilée sans Qt (Win32 pur)
- ⚠️ Gestion du cycle de vie (cleanup à la déconnexion)

## Utilisation Prévue

```cpp
// Dans ApplicationController ou WebView2Inspector

// 1. Tenter connexion TCP directe (pour les apps sans AppContainer)
CdpClient client;
if (client.connectTo("ws://127.0.0.1:9333/devtools/browser/...")) {
    // OK, utiliser client
}

// 2. Sinon, injecter le helper et utiliser le pipe
else {
    quint32 pid = getTargetProcessId();
    QString pipeName = CdpPipeBridge::makePipeNameForProcess(pid);
    
    // Injecter la DLL
    if (launchCdpPipeHelper(pid, pipeName)) {
        // Attendre que le pipe soit créé
        QThread::msleep(500);
        
        // Se connecter via pipe
        CdpPipeBridge bridge;
        if (bridge.connectToServer(pipeName)) {
            // Utiliser bridge (même API que CdpClient)
            auto result = bridge.evaluateJavaScript("document.title");
        }
    }
}
```

## Notes pour Claude

Cette solution est plus robuste que :
- **Hook réseau** : moins intrusif, pas de modification du comportement réseau
- **API native ICoreWebView2** : pas besoin de COM, fonctionne avec tout WebView2
- **Processus intermédiaire** : plus simple, pas besoin de gérer un processus séparé

Le code est prêt pour la partie KillEngine. Il reste à implémenter la DLL helper côté injection.
