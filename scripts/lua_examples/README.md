# KillEngine Lua examples

Ces exemples sont volontairement courts et prudents. Ils servent a tester le
helper `scripts/killengine.lua`, l'onglet `Lua` et la future regression V1 sans
declencher d'ecriture memoire ni de patch fichier.

## Preparation

Depuis la racine du depot :

```powershell
$env:KILLENGINE_AUTOMATION_PIPE = "1"
.\build\bin\KillEngine.exe
```

Dans un autre terminal :

```powershell
$env:KILLENGINE_ROOT = (Get-Location).Path
.\runtime\lua\lua.exe .\scripts\lua_examples\01_ping_and_status.lua
```

Les scripts peuvent aussi etre colles dans l'onglet `Lua` de KillEngine.

Validation rapide :

```powershell
.\scripts\test-lua-examples.ps1
```

Le script valide la presence du runtime, la syntaxe des exemples et une
execution courte de `03_cancellable_wait.lua`. Si KillEngine est deja lance
avec `KILLENGINE_AUTOMATION_PIPE=1`, il lance aussi un smoke du pipe via
`01_ping_and_status.lua`. Ajouter `-RequirePipe` pour rendre ce smoke
obligatoire.

## Scripts

- `01_ping_and_status.lua` : verifie le pipe et affiche le statut Lua expose par le backend.
- `02_exact_scan_snapshot.lua` : lance un scan exact read-only puis affiche un petit apercu des candidats.
- `03_cancellable_wait.lua` : tourne lentement pendant quelques secondes pour verifier le bouton `Stop`.

Variables utiles :

- `KILLENGINE_ROOT` : racine du depot ou du package, utilisee pour trouver `scripts/killengine.lua`.
- `KILLENGINE_SCAN_VALUE` : valeur du scan de l'exemple 02, par defaut `40`.
- `KILLENGINE_SCAN_TYPE` : type du scan de l'exemple 02, par defaut `Int32`.
- `KILLENGINE_LUA_WAIT_SECONDS` : duree de l'exemple annulable, par defaut `30`.
