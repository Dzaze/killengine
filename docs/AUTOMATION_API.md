> **Public visé : utilisateurs avancés / ingénieurs.** Ce document décrit une capacité qui **contourne délibérément les confirmations RiskGate** de KillEngine. Ne l'active que si tu comprends ce que ça implique — voir "Ce que ça change" ci-dessous.

# Automation API

KillEngine expose son moteur complet (attach, scan, lecture/écriture mémoire, freeze, debugger, patchs de code, Trainer...) via un pipe local JSON-RPC. C'est ce qui fait tourner le scripting Lua livré (`ke.call(...)` dans l'onglet **Lua**), et c'est directement utilisable depuis n'importe quel script ou agent externe sur la même machine — y compris un agent IA (Claude Code, ou autre) piloté depuis un terminal.

## Ce que ça change

Le pipe **exécute les actions immédiatement, sans passer par les popups de confirmation** que voit un utilisateur dans l'UI normale (RiskGate). C'est un choix de conception assumé, pas un oubli : un ingénieur qui active ce mode sait déjà ce qu'il écrit, il n'a pas besoin qu'on le lui redemande à chaque appel.

Ce que ça ne change pas :
- Le pipe n'écoute que sur cette machine (named pipe Windows local, aucun accès réseau).
- Chaque appel est journalisé côté KillEngine (`logAiAudit`/télémétrie), même sans popup.
- Le mode reste **désactivé par défaut** : il faut l'activer explicitement.

## Activer le mode Automation

Deux façons équivalentes :

1. **Recommandé — dans l'app** : Paramètres → section "Mode Automation (avancé)" → "Activer le mode Automation". Un unique accord de confirmation apparaît (une fois, pas à chaque action ensuite) ; le pipe démarre immédiatement, sans redémarrer KillEngine.
2. **Dev / scripté** : lancer KillEngine avec la variable d'environnement `KILLENGINE_AUTOMATION_PIPE=1` avant le démarrage.

Le statut (actif ou non, nombre d'appels reçus, dernier appel) reste visible dans ce même panneau Paramètres.

## Protocole

Une ligne JSON par requête, une ligne JSON par réponse (newline-delimited), sur le named pipe `KillEngineAutomationPipe` :

```json
-> {"id":1,"method":"attachProcess","params":[12345]}
<- {"id":1,"result":true}
```

`method` correspond exactement au nom d'une méthode `Q_INVOKABLE` du contrôleur applicatif de KillEngine — la surface complète, exposée par réflexion (pas une liste figée à la main). `params` est un tableau JSON positionnel.

Depuis PowerShell, `scripts/automation-pipe-call.ps1` fait l'aller-retour pour toi :

```powershell
.\scripts\automation-pipe-call.ps1 -Method getProcesses -ParamsJson '[]'
.\scripts\automation-pipe-call.ps1 -Method attachProcess -ParamsJson '[12345]'
.\scripts\automation-pipe-call.ps1 -Method writeMemoryValue -ParamsJson '["7ff711fa10e0","Int32","500"]'
```

### Méthodes courantes

| Méthode | Rôle |
| --- | --- |
| `getProcesses` | Liste les processus (pid, nom, chemin, architecture) |
| `attachProcess(pid)` | Attache au processus |
| `startExactScan(value, valueType)` / `nextScan(mode, value)` | Scan exact et affinages successifs |
| `getCandidates(pageIndex, pageSize, filter)` | Pagine les candidats du scan courant |
| `writeMemoryValue(address, valueType, value)` | Écrit une valeur mémoire |
| `setFreezeValue(address, valueType, value, enable)` | Active/désactive un freeze |
| `findWhatWrites(address, valueType, size, timeoutMs)` | Capture les instructions qui écrivent sur une adresse (breakpoint matériel) |
| `applyCodePatch(addressHex, bytesHex, verify)` / `restoreCodePatch(addressHex)` | Patch de code / restauration |
| `readMemoryKernel(address, size)` / `writeMemoryValueKernel(address, valueType, value)` | Lecture/écriture via driver kernel (si disponible) |

Cette liste est indicative — toute méthode `Q_INVOKABLE` de `ApplicationController` (`apps/desktop/application_controller.h`) est appelable de la même façon.

## Depuis un script Lua

Le wrapper `scripts/killengine.lua` encapsule le protocole :

```lua
local ke = require("killengine")

ke.attach(12345)
local scan = ke.scan_exact("100", "Int32")
ke.kernel_write_value("7ff711fa10e0", "Int32", "500")
```

Chaque `ke.xxx(...)` est un appel `ke.call("methodName", {...})` en dessous — voir `scripts/lua_examples/` pour des scripts complets.

## Brancher son propre agent IA

C'est le scénario qui a motivé ce document : un agent IA avec accès terminal (Claude Code ou équivalent) peut piloter KillEngine de bout en bout — pas seulement appeler le pipe, mais aussi driver l'UI réelle (chat Assistant, navigation) via le Chrome DevTools Protocol exposé par QtWebEngine.

1. **Lancer KillEngine avec le mode Automation actif** (toggle Paramètres, ou `KILLENGINE_AUTOMATION_PIPE=1`) et, si besoin de piloter l'UI en plus du moteur, avec `QTWEBENGINE_REMOTE_DEBUGGING=127.0.0.1:<port>` défini avant le lancement.
2. **Piloter le moteur directement** via `automation-pipe-call.ps1` (attach, scan, write, freeze...) — c'est le chemin le plus direct pour un agent qui n'a besoin que des résultats, pas de l'UI.
3. **Piloter l'UI réelle** (utile pour vérifier qu'un flux fonctionne de bout en bout, y compris le chat de l'Assistant) via CDP :
   - `GET http://127.0.0.1:<port>/json` → récupère `webSocketDebuggerUrl`.
   - `Runtime.evaluate` pour taper dans un champ (setter natif + `Event('input',{bubbles:true})` pour que Vue le voie), cliquer un bouton, lire `document.body.innerText`.
   - `Page.captureScreenshot` pour une preuve visuelle.

Exemple minimal (PowerShell, `System.Net.WebSockets.ClientWebSocket`) — taper une adresse dans le chat Assistant et cliquer "Rechercher" :

```powershell
$ws = New-Object System.Net.WebSockets.ClientWebSocket
$ws.ConnectAsync([Uri]"ws://127.0.0.1:9333/devtools/page/<id>", [Threading.CancellationToken]::None).GetAwaiter().GetResult()

$js = '(function(){var i=document.querySelector("input.chat-input");' +
      'var s=Object.getOwnPropertyDescriptor(HTMLInputElement.prototype,"value").set;' +
      's.call(i,"0x7ff711fa10e0");i.dispatchEvent(new Event("input",{bubbles:true}));' +
      'document.querySelector(".input-bar button").click();return "ok";})()'
$payload = @{ id=1; method="Runtime.evaluate"; params=@{ expression=$js; returnByValue=$true } } | ConvertTo-Json -Compress
$bytes = [Text.Encoding]::UTF8.GetBytes($payload)
$ws.SendAsync([ArraySegment[byte]]::new($bytes), 'Text', $true, [Threading.CancellationToken]::None).GetAwaiter().GetResult()
```

Ni le pipe ni le CDP ne sont spécifiques à KillEngine — ce sont des primitives génériques (named pipe JSON-RPC, Chrome DevTools Protocol) que n'importe quel agent avec accès shell sait déjà utiliser une fois qu'il connaît le protocole ci-dessus.

## Limites volontaires

- Aucune authentification au-delà de "processus local sur la même machine" (le pipe Windows lui-même n'est pas accessible à distance).
- Aucune confirmation par action une fois le mode actif — c'est le point du mode Automation, pas un bug.
- Le protocole JSON-RPC positionnel (`params` en tableau, pas en objet nommé) suit exactement la signature C++ de chaque méthode — se référer à `apps/desktop/application_controller.h` pour les types attendus.
