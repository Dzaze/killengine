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

### Gestion des profils (`.keprofile`, module `apps/desktop/profile_manager.h/.cpp`, PHASE 231)

Cluster vérifié en direct via le pipe (`scripts/test-automation-pipe-profile-methods.ps1`, voir plus bas) — formes de réponse copiées depuis le code réel, pas devinées :

| Méthode | Params | Champs clés de la réponse |
| --- | --- | --- |
| `saveProfileTarget(profileName, targetName, addressHex, valueType, description)` | 5 strings | `success`, `targetName`, `locator`, `locatorKind` (`"module_offset"` ou `"absolute"`), `module`, `offset`, `targetCount` |
| `loadProfile(profileName)` | 1 string | `success`, `gameName`, `executableName`, `targets` (liste : `name`/`type`/`locator`/`locatorKind`/`description`/`dependsOn`), `targetCount`, `patches`, `patchCount`, `autoAsmScripts`, `luaScripts` |
| `resolveProfileTarget(profileName, targetName)` | 2 strings | `success`, `address` (hex **sans** `0x`), `type`, `locator`, `locatorKind` — si `locatorKind == "clr_field"` : `clrTypeSubstring`/`clrIdentityField`/`clrIdentityValue`/`clrFieldName` en plus |
| `comparePointerMapAcrossRestart(profileName)` | 1 string | `success`, `validCount`, `invalidCount`, `unsupportedCount`, **`results`** (liste : `targetName`/`locatorKind`/`previousAddress`/`address`/`status` `"valid"` ou `"invalid"`) — **pas** `entries`, piège déjà fait une fois (PHASE 231) |
| `deleteProfile(profileName)` | 1 string | booléen brut, pas un objet |
| `listProfiles()` | aucun | liste de profils (pas un objet englobant) |

### Memory Timeline (`core/visualization/memory_timeline_*`, PHASES 200-203 + analyzer 03/09/2026)

Absente de `docs/AUTOMATION_API_REFERENCE.md` (snapshot généré le 30/08/2026, avant cette famille de méthodes) — table ajoutée ici pour qu'un agent piloté par le pipe la découvre sans avoir à deviner. Formes de réponse copiées depuis `apps/desktop/application_controller.cpp`/`memory_timeline_manager.cpp` et vérifiées live via `KillEngineTestTarget.exe`.

| Méthode | Params | Champs clés de la réponse |
| --- | --- | --- |
| `addTimelineAddress(addressHex, valueSize)` / `removeTimelineAddress(addressHex)` / `clearTimelineAddresses()` | hex string + int, ou rien | `success` (+ `error` si adresse invalide) |
| `getTimelineWatchedAddresses()` | aucun | `success`, `addresses` (liste hex) |
| `setTimelineConfig({samplingIntervalMs?, maxDurationMs?, trackOnlyChanges?})` / `getTimelineConfig()` | objet partiel, ou rien | `success`, `config` (`samplingIntervalMs`, `maxDurationMs`, `maxPointsPerSeries`, `trackOnlyChanges`, `calculateStatistics`) |
| `startTimelineCollection()` / `stopTimelineCollection()` | aucun | `success` (+ `error` si aucun processus attaché) |
| `getTimelineStatus()` | aucun | `success`, `collecting`, `watchedAddressCount`, `stats` |
| `getTimelineSeriesForAddress(addressHex)` | hex string | `success`, `series` (`address`, `valueSize`, `changeCount`, `averageIntervalMs`, `volatilityScore`, `points` : liste `{timestampMs, valueHex, isValid}`) |
| `getAllTimelineSeries()` | aucun | `success`, `series` (liste du format ci-dessus) |
| `findVolatileTimelineAddresses(threshold)` / `findStableTimelineAddresses(minDurationMs)` | double / int | `success`, `addresses` |
| `exportTimelineToJson()` / `exportTimelineToCsv()` | aucun | `success`, `filepath` (+ `error` si échec) |
| `detectTimelinePatterns(addressHex)` | hex string | `success`, `address`, `patterns` (liste : `type` ex. `step_function`/`cyclic`/`linear`, `confidence`, `description`, `correlationScore`, `periodMs`, `slope`) — peut être `[]` si aucun pattern au-dessus du seuil de confiance |
| `analyzeTimelineBehavior(addressHex)` | hex string | `success`, `behavior` (`changesPerSecond`, `regularityScore`, `distinctValueCount`, `minValueHex`, `maxValueHex`, `mostCommonValueHex`, `typicalResponseTimeMs`, `hasBurstBehavior`) |
| `predictTimelineNextValue(addressHex)` | hex string | `success`, `address`, `valueHex`, `changeProbability` |
| `findTimelineCorrelations()` (ajouté 03/09/2026) | aucun | `success`, `correlations` (liste : `addressA`, `addressB`, `pearsonCoefficient`, `timeLagMs`, `isLeading`) — compare toutes les paires parmi les séries actuellement collectées |
| `generateTimelineReport()` (ajouté 03/09/2026) | aucun | `success`, `report` (texte résumant patterns/comportement par série) |

### Référence exhaustive de toutes les méthodes

`docs/AUTOMATION_API_REFERENCE.md` (PHASE 238, 30/08/2026) inventorie la forme de réponse des ~200 méthodes `Q_INVOKABLE` restantes, groupées par domaine (scan, write/freeze, trainer, CLR, patch/AOB, debug, settings/diagnostics, workspace/pointer chains, Lua, chat/IA, injection, save-file, kernel, réseau/speedhack). Fait une fois le refactor entièrement clos (backend C1-C14, frontend S1-S12), condition explicitement posée en PHASE 233 pour ne pas documenter une surface encore en mouvement. **Reste un instantané figé à sa date de génération** — une méthode modifiée après coup (un chantier en cours au moment de la génération, ex. `inferStructureInstanceDelta`, est marqué comme tel dans le doc) doit être revérifiée avec la méthode ci-dessous plutôt que de faire aveuglément confiance à l'instantané.

### Où trouver la forme exacte d'une réponse pas (ou plus) à jour

`application_controller.h` donne la signature (types des **paramètres**), mais **pas** la forme du `QVariantMap` retourné — ça a déjà fait perdre du temps à un agent qui devinait (PHASE 231/232). Le réflexe qui marche à tous les coups et ne devient jamais obsolète, à utiliser pour toute méthode ajoutée/modifiée après la génération de `docs/AUTOMATION_API_REFERENCE.md` :

```powershell
# Cherche directement la construction de la réponse dans le manager concerné.
# Ex. pour une méthode de profil : apps/desktop/profile_manager.cpp
Select-String -Path apps\desktop\profile_manager.cpp -Pattern 'result\["\w+"\]\s*='
```

Si la méthode n'a pas encore été extraite dans un `*_manager.cpp` dédié (voir `docs/REFACTOR_ROADMAP.md` pour la liste), elle vit encore dans `apps/desktop/application_controller.cpp` — même recherche, même fichier.

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

## Vérifier que le pipe fonctionne (avant de scripter dessus)

Deux batteries jetables existent déjà, contre un vrai couple `KillEngineTestTarget.exe` + `KillEngine.exe` (`KILLENGINE_AUTOMATION_PIPE=1`) — les lancer d'abord évite de perdre du temps à deviner si un problème vient du pipe ou de son propre script :

```powershell
.\scripts\test-automation-pipe-safe-methods.ps1     # 12 méthodes lecture seule (ping, attach, readMemoryPreview...)
.\scripts\test-automation-pipe-profile-methods.ps1  # cycle profil complet : save -> load -> resolve -> compare -> delete
```

Les deux nettoient leurs deux processus dans un bloc `finally` même en cas d'échec. Elles servent aussi de référence exécutable pour la forme exacte des réponses (voir tableau ci-dessus).

## Limites volontaires

- Aucune authentification au-delà de "processus local sur la même machine" (le pipe Windows lui-même n'est pas accessible à distance).
- Aucune confirmation par action une fois le mode actif — c'est le point du mode Automation, pas un bug.
- Le protocole JSON-RPC positionnel (`params` en tableau, pas en objet nommé) suit exactement la signature C++ de chaque méthode — se référer à `apps/desktop/application_controller.h` pour les types attendus.
