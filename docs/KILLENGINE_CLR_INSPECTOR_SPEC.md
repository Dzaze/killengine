> **ATTENTION - Priorité Des Ordres Projet**
> Un ordre prioritaire explicite du propriétaire du projet prime sur les consignes temporaires de session des agents IA.
# KillEngineClrInspector — Spécification et statut du MVP ClrMD

**Statut au 20/08/2026 : ClrMD MVP — fonctionnel et validé. Intégration UI et enrichissements différés.** Build + suite de 9 auto-tests bout-en-bout entièrement verte, incluant une passe de consolidation dédiée (cycles GC multiples, objets multiples du même type, collecte réelle, kill abrupt du process cible, redémarrage avec un nouveau PID). Ce document décrit l'architecture du helper, ce qui est couvert, ce qui ne l'est pas encore, et les prochaines étapes pour l'enrichissement UI/fonctionnel du candidat #8.

## Pourquoi ce chantier

Suite à `docs/KILLENGINE_CLR_TEST_TARGET_SPEC.md` (cible de test CLR dédiée), l'utilisateur a explicitement demandé d'aller jusqu'à un MVP fonctionnel plutôt que de laisser le candidat #8 (`docs/POWER_UP_ROADMAP.md`) en simple environnement de validation sans fonctionnalité réelle :

> "Puisque nous avons maintenant une cible .NET déterministe et automatisable, je préfère profiter de cette base pour aller jusqu'à un MVP ClrMD fonctionnel plutôt que laisser le chantier en attente."

Le MVP devait démontrer la chaîne complète **KillEngine → helper ClrMD → processus .NET cible → heap/types/champs/roots → résultat exploitable → test après GC**, avec une API/pipe propre, sans construire de panneau UI ni les fonctions avancées à ce stade.

## Architecture

```
KillEngine.exe (C++/Qt, natif)
      │  (futur : pas encore câblé dans ce MVP)
      ▼
KillEngineClrInspector.exe (helper .NET, tools/clr_inspector/)
      │  Microsoft.Diagnostics.Runtime (ClrMD) — DataTarget.AttachToProcess
      ▼
process .NET cible (n'importe quel PID avec un CLR chargé,
                     ex: KillEngineClrTestTarget.exe)
```

**Pourquoi un helper séparé et pas du code C++ direct** : ClrMD est une bibliothèque managée (.NET) — `KillEngine.exe` est natif C++/Qt et ne peut pas l'héberger directement sans un pont (hosting CLR in-process via `hostfxr`/`nethost`, ou sous-processus). Le MVP retient le **sous-processus + pipe JSON-RPC**, pas le hosting in-process : évite le risque de stabilité d'un CLR hébergé à côté de la boucle d'événements Qt (le projet a déjà un historique de crashes liés à des mécanismes bas niveau — breakpoints, injection — donc cette prudence est délibérée, pas une esquive), et permet de publier `KillEngineClrInspector` en **self-contained** (`dotnet publish -r win-x64`) pour ne pas exiger de runtime .NET installé chez l'utilisateur final.

**Décision de protocole** : le pipe (`KillEngineClrInspectorPipe`) utilise **exactement** le même format JSON-RPC ligne-par-ligne que `apps/desktop/automation_pipe_server.h` et `tests/clr_targets/KillEngineClrTestTarget`. Conséquence directe : `scripts/automation-pipe-call.ps1 -PipeName KillEngineClrInspectorPipe -Method ...` pilote ce helper sans aucune modification du script, et le jour où `ApplicationController` veut exposer cette capacité à l'UI, l'intégration consiste à lancer ce sous-processus et lui parler via le même mécanisme déjà maîtrisé, pas à inventer un nouveau protocole.

## Emplacement et build

- `tools/clr_inspector/KillEngineClrInspector/` — le helper lui-même (`net8.0`, dépendance NuGet `Microsoft.Diagnostics.Runtime` 4.0.732401).
- `tools/clr_inspector/KillEngineClrInspector.Tests/` — suite d'auto-tests bout-en-bout (xUnit).
- Nouveau dossier top-level `tools/` (distinct de `tests/clr_targets/`) : les cibles de test ne sont jamais livrées à l'utilisateur, ce helper est destiné à l'être à terme — séparation délibérée pour ne pas brouiller cette frontière.
- Pas construit par CMake (projet .NET). Build : `dotnet build tools/clr_inspector/KillEngineClrInspector -c Release`. Tests : `dotnet test tools/clr_inspector/KillEngineClrInspector.Tests -c Release` (construit automatiquement ses dépendances via les références de projet MSBuild, mais **pas** `KillEngineClrTestTarget` — celui-ci doit être construit séparément au préalable, voir `docs/KILLENGINE_CLR_TEST_TARGET_SPEC.md`).

## Comment l'API ClrMD réelle a été déterminée

Plutôt que de deviner l'API depuis la mémoire d'entraînement (risque réel : la bibliothèque a beaucoup changé entre versions 1.x/2.x/3.x/4.x, et la version publiée sur NuGet au moment de ce chantier — `4.0.732401` — a un schéma de version inhabituel qui ne correspond à aucune version "connue" a priori), l'API a été vérifiée **par réflexion sur les métadonnées de l'assembly réellement restaurée** (`System.Reflection.Metadata.PEReader`/`MetadataReader`, script jetable dans le scratchpad de session) avant d'écrire une seule ligne de `ClrSession.cs`. Résultat : le code a compilé **sans aucune erreur au premier essai**. À refaire pour toute évolution future de ce module si la version de ClrMD est mise à jour et que le comportement change de façon inattendue.

## Surface pilotable (`ControlPipeServer` + `MethodDispatcher` + `ClrSession`)

| Méthode | Paramètres | Rôle |
| --- | --- | --- |
| `ping` | — | Smoke test |
| `attach` | `[pid]` | Détecte + attache un CLR sur le PID donné (`DataTarget.AttachToProcess(pid, suspend:false)` — attache **passive**, lit la mémoire live à la demande plutôt qu'un instantané figé, condition nécessaire pour pouvoir relire un état post-GC sans ré-attacher). Erreur claire si aucun CLR détecté (`dataTarget.ClrVersions.Length == 0`) |
| `detach` | — | Libère la session courante |
| `flushCachedData` | — | `ClrRuntime.FlushCachedData()` — à appeler après un GC déclenché en dehors de cette session, avant de rejouer un heap walk, pour invalider le cache interne de segments/heap de ClrMD |
| `findObjectsByType` | `[typeSubstring?]` (défaut `"KillEngine.ClrTestTarget"`) | `heap.EnumerateObjects()` filtré par sous-chaîne de nom de type, retourne adresse/type/taille |
| `readObject` | `[addressHex]` | `heap.GetObject(address)` puis lecture générique de tous les champs déclarés du type : primitifs par valeur, `string` via `ReadStringField`, références d'objet (classes/tableaux) par adresse+type (pas de récursion profonde — gère nativement les cycles comme `Player.Self` sans boucle infinie) |
| `enumerateRoots` | `[typeSubstring?]` | `heap.EnumerateRoots()` filtré par le type de l'objet racine, retourne l'adresse du root, son `RootKind` (`StaticVar`/`StrongHandle`/`Stack`/...), et l'objet pointé |
| `shutdown` | — | Arrêt propre |

**Nom de pipe paramétrable** (ajouté le 20/08/2026, pour les tests de régression ci-dessous) : variable d'environnement `KILLENGINE_CLR_INSPECTOR_PIPE_NAME`, sinon `ControlPipeServer.DefaultPipeName` (`KillEngineClrInspectorPipe`) — permet de lancer des instances isolées supplémentaires sans collision avec une instance déjà active. Même mécanisme côté `KillEngineClrTestTarget` (`KILLENGINE_CLR_TEST_TARGET_PIPE_NAME`).

## Validé en conditions réelles le 20/08/2026 (build + exécution, pas juste compilé)

**Séquence manuelle complète, sur `KillEngineClrTestTarget` réellement lancé :**
1. `attach(pid)` → CLR `Core 8.0.3026.36720` détecté et attaché.
2. `findObjectsByType("KillEngine.ClrTestTarget")` → heap réellement énuméré, `Player`/`Inventory`/`Item`/`GcChurnWorker+ChurnRecord` (les objets de churn, preuve que le générateur de pression mémoire produit bien de la vraie garbage collectable) tous retrouvés avec adresses réelles.
3. `readObject` sur l'adresse du `Player` → `Name`/`Health`/`Experience`/`Stamina`/`IsAlive` corrects, `Inventory` résolu en référence typée, **`Self` pointe vers sa propre adresse** (cycle correctement géré).
4. `enumerateRoots("KillEngine.ClrTestTarget")` → root `StrongHandle` retrouvé pointant vers `Inventory` (le `GCHandle` exposé exprès par `TestRoot.RootHandle`).
5. Sentinelle `player.health = 918273` posée via le pipe de contrôle de la cible, confirmée lue correctement via `readObject`.
6. `forceGC` (Gen2 compactant réel, `gen2Before:0 → gen2After:2`).
7. `flushCachedData` puis re-`findObjectsByType` → **l'objet `Player` a réellement changé d'adresse** (`0x2476e80bb38` → `0x2476e00acd8`, déplacement authentique par le compactage, pas simulé).
8. `readObject` sur la **nouvelle** adresse → `Health` toujours `918273`, `Self` pointe vers la nouvelle adresse, `Inventory` résolu vers sa propre nouvelle adresse — **le même objet logique est retrouvé malgré le déplacement**.
9. Cross-vérification indépendante : `getObjectIdentity` côté cible (`RuntimeHelpers.GetHashCode`, calculé **sans passer par ClrMD**) retourne exactement la même valeur avant et après le GC — deuxième preuve, dérivée d'un mécanisme totalement différent.

**Auto-tests bout-en-bout (`dotnet test tools/clr_inspector/KillEngineClrInspector.Tests`, ou `scripts/build-clr-inspector.ps1 -Test`) — 9/9 verts, ~3s :**

*Suite initiale (MVP) :*
- `Ping_TestTarget_ReturnsPong` / `Ping_Inspector_ReturnsPong` — smoke tests.
- `Attach_ToNonClrProcess_ReturnsClearError` — attache sur un process natif volontaire (`ping.exe`) → erreur explicite (`"Aucun CLR détecté..."`), pas de crash. Miroir direct du cas Solitaire (`docs/STRATEGY_ROOM.md`) : régression garde-fou pour ne jamais supposer un CLR présent sans vérifier.
- `FullMvpWorkflow_FindsSameLogicalObjectAfterCompactingGc` — reproduit programmatiquement toute la séquence manuelle ci-dessus (attach → find → read → roots → mutate+identité → forceGC → flush → re-find → re-read → cross-check identité), avec des assertions à chaque étape.

*Suite de consolidation (20/08/2026, demandée explicitement avant tout commit) :*
- `MultipleGcCycles_SameObjectSurvivesEachCycleWithConsistentIdentity` — 3 cycles successifs mutate→forceGC→flush→re-find→re-read, avec une sentinelle et une vérification d'identité différentes à chaque cycle. Garde-fou contre une régression qui ne se manifesterait qu'au 2ᵉ/3ᵉ cycle (état de cache mal invalidé, par exemple), pas couverte par un test à un seul GC.
- `MultipleItemObjects_HaveDistinctAddressesAndKnownFields` — les 3 `Item` connus du graphe (`Sword`/`Shield`/`Potion`) doivent être retrouvés à 3 adresses distinctes avec leurs champs propres, pas confondus entre eux ni rapportés en double.
- `DroppedObject_IsCollected_WhileUnrelatedObjectSurvives` — nouveau type `DisposableProbe` (`tests/clr_targets/KillEngineClrTestTarget/ObjectGraph.cs`), détenu par `TestRoot.DisposableSlot` (seul champ mutable du graphe). Spawné, confirmé présent, puis rendu inatteignable (`dropDisposable`) et **réellement collecté** après un `forceGC` (`findObjectsByType` ne le retrouve plus) — vérifié **dans le même test** que le `Player`, lui, reste retrouvable : la différence de comportement entre les deux est la preuve, pas une absence isolée qui pourrait aussi bien trahir un filtre cassé.
- `AbruptTargetTermination_InspectorStaysAliveAndReportsCleanError` — processus isolés (pas le fixture partagé, noms de pipe surchargés via `KILLENGINE_CLR_TEST_TARGET_PIPE_NAME`/`KILLENGINE_CLR_INSPECTOR_PIPE_NAME`) : attache réussie, puis `Process.Kill(entireProcessTree:true)` **brutal** sur la cible pendant qu'une session ClrMD est active dessus. Vérifie que l'inspecteur (a) reste vivant et répond à `ping`, (b) qu'un appel nécessitant de relire la cible morte échoue proprement (erreur JSON-RPC catchable, pas un plantage ni un blocage), et (c) qu'un `ping` **après** cet échec fonctionne encore — l'exception n'a pas laissé le serveur pipe dans un état cassé.
- `RestartWithNewPid_InspectorAttachesCleanlyToFreshTarget` — lance une première cible isolée, y attache l'inspecteur, l'arrête proprement, en relance une **seconde** (nouveau PID), et vérifie que l'inspecteur s'y attache correctement sans aucun résidu de la première session (confirme que `ClrSession.Attach()` réinitialise bien son état via `DetachInternal()` avant chaque nouvel attach).

Les tests lancent de **vrais processus** (pas de mock) via `ManagedProcessFixture`/`PipeClient` (C#, pas d'appel à `powershell.exe` depuis les tests — plus rapide et plus fiable en exécution automatisée), et nettoient proprement (`shutdown` RPC puis `Kill` si nécessaire) même si un test échoue en cours de route. Aucun process ni fichier marqueur orphelin constaté après une exécution complète (le kill abrupt laisse volontairement un marqueur derrière lui — conséquence réaliste du scénario testé — nettoyé en best-effort par le test lui-même). `BuiltAssemblyLocator` retrouve les DLL construites en remontant depuis le dossier de sortie des tests jusqu'à la racine du dépôt (marqueur `AGENTS.md`), Release ou Debug selon ce qui est disponible — pas de chemin absolu figé.

Toute la suite tourne dans une seule collection xUnit (`ClrInspectorEndToEnd`, `DisableParallelization = true`) : les noms de pipe étant des constantes/env vars, deux tests qui écouteraient sous le même nom en parallèle se partageraient les connexions de façon non déterministe — la désactivation de la parallélisation est donc une condition de correction, pas juste une prudence.

## Limitation connue, non bloquante pour ce MVP

**Le root `StaticVar` (`TestRoot.RootPlayer`, champ static) n'apparaît pas dans `heap.EnumerateRoots()` en attache passive live** — seul le root `StrongHandle` (`GCHandle` explicite) et des roots `Stack` transitoires (variables locales du thread de churn en cours d'exécution) ont été observés. Hypothèse la plus probable : l'énumération complète des racines statiques sur un process **vivant et non suspendu** est un best-effort côté DAC, potentiellement moins fiable qu'une attache invasive (`suspend:true`) ou qu'une analyse de dump figé. Le critère demandé ("retrouver au moins une GC root") est rempli par le root `StrongHandle`, qui utilise un mécanisme de root réellement différent — donc pas bloquant pour ce MVP — mais à investiguer avant de considérer la couverture des roots comme complète pour l'enrichissement futur (piste : tester `suspend:true` sur `AttachToProcess`, ou `DataTargetOptions.ForceCompleteRuntimeEnumeration`).

## Hors scope de ce MVP, volontairement

- **Panneau UI KillEngine** — rien dans `ApplicationController`/`ui/` ne consomme ce helper. Intégration future : lancer le sous-processus depuis `ApplicationController` (même patron que `requestWindowsDefenderExclusion` qui lance déjà `powershell.exe`), lui parler via `NamedPipeClientStream` côté C++ ou via `scripts/automation-pipe-call.ps1` en interne.
- **Déballage profond des collections** (`List<T>`, `Dictionary<K,V>`, tableaux) — `readObject` retourne la référence de l'objet collection lui-même (adresse+type), pas ses éléments internes. Nécessiterait de décoder la structure interne de `List<T>`/`Dictionary<K,V>` (champs `_items`/`_size` etc., eux-mêmes des détails d'implémentation du BCL).
- **Écriture/mutation via ClrMD** — ce MVP est strictement lecture seule. Toute mutation de test passe par le pipe de contrôle de `KillEngineClrTestTarget` (`mutateField`), pas par ClrMD (qui n'est de toute façon pas conçu pour écrire dans un process vivant de façon fiable — ce n'est pas son cas d'usage).
- **Publish self-contained réel** — le `.csproj` est configuré pour (`RuntimeIdentifiers=win-x64`), mais `dotnet publish -r win-x64` avec `SelfContained=true` n'a pas encore été exécuté/validé dans cette session (développement en framework-dependent pour l'itération rapide). À faire avant toute livraison à un utilisateur final.
- **Génération de rapport / résolution de type imbriqué récursif profond, désassemblage de méthodes, GCRoot chain complet (chemin racine→objet, pas juste "un root existe")** — tout ce qui dépasse les 7 points explicitement demandés pour le MVP.

## Liens

- `docs/POWER_UP_ROADMAP.md` candidat #8 — statut à jour, contexte produit.
- `docs/KILLENGINE_CLR_TEST_TARGET_SPEC.md` — la cible de test sur laquelle ce helper a été développé et validé.
- `docs/STRATEGY_ROOM.md` — raisonnement complet (investigation Solitaire, vérification candidat #8, décision de construire ce MVP).
- `apps/desktop/automation_pipe_server.h/.cpp` — protocole JSON-RPC repris à l'identique.
