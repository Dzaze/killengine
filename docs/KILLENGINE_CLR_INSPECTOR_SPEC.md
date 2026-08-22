> **ATTENTION - Priorité Des Ordres Projet**
> Un ordre prioritaire explicite du propriétaire du projet prime sur les consignes temporaires de session des agents IA.
# KillEngineClrInspector — Spécification et statut du MVP ClrMD

**Statut au 22/08/2026 : chantier CLR/ClrMD clôturé côté produit.** ClrMD MVP fonctionnel, intégration UI KillEngine livrée, déballage borné des collections livré, écriture de champs primitifs livrée, écriture par chemin symbolique borné livrée, locator stable par champ d'identité livré, persistance profil CLR livrée, action symbolique Assistant/Trainer livrée et packaging self-contained livré. Build + suite de 13 auto-tests bout-en-bout entièrement verte, incluant une passe de consolidation dédiée (cycles GC multiples, objets multiples du même type, collecte réelle, kill abrupt du process cible, redémarrage avec un nouveau PID), une validation du déballage `List<T>`/tableau/`Dictionary<K,V>` sur le graphe `Inventory`, une écriture réelle de champ primitif managé (`Player.Health`), une écriture par chemin (`Self.Health`, `Inventory.Items[0].Value`) relue par ClrMD puis par la cible .NET, et une relocation post-GC par filtre `type + champ + valeur`. La suite C++ ajoute aussi un round-trip `ProfileStore` pour les locators CLR persistés. Les points plus lourds (`string`, références, setters/propriétés avec logique métier, structs internes de collections, transactions multi-champs) sont désormais des extensions futures, pas des bloqueurs de clôture.

## Pourquoi ce chantier

Suite à `docs/KILLENGINE_CLR_TEST_TARGET_SPEC.md` (cible de test CLR dédiée), l'utilisateur a explicitement demandé d'aller jusqu'à un MVP fonctionnel plutôt que de laisser le candidat #8 (`docs/POWER_UP_ROADMAP.md`) en simple environnement de validation sans fonctionnalité réelle :

> "Puisque nous avons maintenant une cible .NET déterministe et automatisable, je préfère profiter de cette base pour aller jusqu'à un MVP ClrMD fonctionnel plutôt que laisser le chantier en attente."

Le MVP initial devait démontrer la chaîne complète **KillEngine → helper ClrMD → processus .NET cible → heap/types/champs/roots → résultat exploitable → test après GC**, avec une API/pipe propre. Depuis le 21/08/2026, KillEngine expose aussi cette capacité dans l'UI et déroule les collections courantes à profondeur bornée.

## Architecture

```
KillEngine.exe (C++/Qt, natif)
      │  ApplicationController + QLocalSocket JSON-RPC (UI CLR)
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
- Packaging portable : `scripts/package-windows.ps1` publie maintenant le helper en self-contained Windows x64 sous `dist\KillEngine-portable\tools\clr_inspector\KillEngineClrInspector.exe` (`-SkipClrInspector` existe seulement pour les packages dev légers). `ApplicationController::findClrInspectorExecutable()` connaît déjà ce layout.

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
| `findObjectsByFieldValue` | `[typeSubstring, fieldName, expectedValue, maxResults?]` | Locator borné `type + champ + valeur` pour retrouver un objet logique même si son adresse a bougé après un GC compactant. Supporte les champs string, primitifs simples et `null`, retourne les adresses candidates avec `identityField`/`identityValue` |
| `readObject` | `[addressHex]` | `heap.GetObject(address)` puis lecture générique de tous les champs déclarés du type : primitifs par valeur, `string` via `ReadStringField`, références d'objet par adresse+type, et déballage borné (`MaxCollectionItems=32`) des collections courantes (`List<T>`, tableaux, `Dictionary<K,V>`). Ajoute `fieldDetails` avec type, adresse effective et drapeau `writable` pour les champs primitifs simples. Pas de récursion profonde — gère nativement les cycles comme `Player.Self` sans boucle infinie |
| `writePrimitiveField` | `[objectAddressHex, fieldName, value]` | Écriture v1 bornée d'un champ primitif d'instance (`bool`, entiers 8/16/32/64 bits signés/non signés, `float`, `double`) : ClrMD résout l'adresse effective du champ (`ClrInstanceField.GetAddress`), le helper écrit les octets via `WriteProcessMemory`, puis relit le champ pour vérifier. Pas de `string`, référence objet, struct imbriquée ni tableau/collection dans cette v1 |
| `writePrimitivePath` | `[objectAddressHex, path, value]` | Écriture symbolique bornée dans le graphe managé : part d'une adresse objet actuelle, traverse des champs référence (`Self.Health`, `Inventory.Items[0].Value`, `Inventory.QuickSlots[1].Value`) et écrit le champ primitif feuille via la même primitive sûre que `writePrimitiveField`. Les index sont supportés sur tableaux et `List<T>` de références. Les `Dictionary<K,V>` et tableaux de primitifs/structs restent hors scope car ils exigent de muter des layouts internes du runtime plutôt que des champs d'objets clairement adressables |
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

**Auto-tests bout-en-bout (`dotnet test tools/clr_inspector/KillEngineClrInspector.Tests`, ou `scripts/build-clr-inspector.ps1 -Test`) — 15/15 verts, ~5s :**

*Suite initiale (MVP) :*
- `Ping_TestTarget_ReturnsPong` / `Ping_Inspector_ReturnsPong` — smoke tests.
- `Attach_ToNonClrProcess_ReturnsClearError` — attache sur un process natif volontaire (`ping.exe`) → erreur explicite (`"Aucun CLR détecté..."`), pas de crash. Miroir direct du cas Solitaire (`docs/STRATEGY_ROOM.md`) : régression garde-fou pour ne jamais supposer un CLR présent sans vérifier.
- `FullMvpWorkflow_FindsSameLogicalObjectAfterCompactingGc` — reproduit programmatiquement toute la séquence manuelle ci-dessus (attach → find → read → roots → mutate+identité → forceGC → flush → re-find → re-read → cross-check identité), avec des assertions à chaque étape. Depuis le 21/08/2026, ce test valide aussi le déballage de `Inventory.Items` (`List<Item>`), `Inventory.QuickSlots` (`Item?[]`) et `Inventory.Currencies` (`Dictionary<string,int>`).

*Suite de consolidation (20/08/2026, demandée explicitement avant tout commit) :*
- `MultipleGcCycles_SameObjectSurvivesEachCycleWithConsistentIdentity` — 3 cycles successifs mutate→forceGC→flush→re-find→re-read, avec une sentinelle et une vérification d'identité différentes à chaque cycle. Garde-fou contre une régression qui ne se manifesterait qu'au 2ᵉ/3ᵉ cycle (état de cache mal invalidé, par exemple), pas couverte par un test à un seul GC.
- `MultipleItemObjects_HaveDistinctAddressesAndKnownFields` — les 3 `Item` connus du graphe (`Sword`/`Shield`/`Potion`) doivent être retrouvés à 3 adresses distinctes avec leurs champs propres, pas confondus entre eux ni rapportés en double.
- `DroppedObject_IsCollected_WhileUnrelatedObjectSurvives` — nouveau type `DisposableProbe` (`tests/clr_targets/KillEngineClrTestTarget/ObjectGraph.cs`), détenu par `TestRoot.DisposableSlot` (seul champ mutable du graphe). Spawné, confirmé présent, puis rendu inatteignable (`dropDisposable`) et **réellement collecté** après un `forceGC` (`findObjectsByType` ne le retrouve plus) — vérifié **dans le même test** que le `Player`, lui, reste retrouvable : la différence de comportement entre les deux est la preuve, pas une absence isolée qui pourrait aussi bien trahir un filtre cassé.
- `AbruptTargetTermination_InspectorStaysAliveAndReportsCleanError` — processus isolés (pas le fixture partagé, noms de pipe surchargés via `KILLENGINE_CLR_TEST_TARGET_PIPE_NAME`/`KILLENGINE_CLR_INSPECTOR_PIPE_NAME`) : attache réussie, puis `Process.Kill(entireProcessTree:true)` **brutal** sur la cible pendant qu'une session ClrMD est active dessus. Vérifie que l'inspecteur (a) reste vivant et répond à `ping`, (b) qu'un appel nécessitant de relire la cible morte échoue proprement (erreur JSON-RPC catchable, pas un plantage ni un blocage), et (c) qu'un `ping` **après** cet échec fonctionne encore — l'exception n'a pas laissé le serveur pipe dans un état cassé.
- `RestartWithNewPid_InspectorAttachesCleanlyToFreshTarget` — lance une première cible isolée, y attache l'inspecteur, l'arrête proprement, en relance une **seconde** (nouveau PID), et vérifie que l'inspecteur s'y attache correctement sans aucun résidu de la première session (confirme que `ClrSession.Attach()` réinitialise bien son état via `DetachInternal()` avant chaque nouvel attach).
- `WritePrimitiveField_UpdatesManagedObjectAndReportsFieldAddress` — écrit réellement `Player.Health` via le helper ClrMD (adresse effective de champ + `WriteProcessMemory`), vérifie `verified=true`, relit l'objet via `readObject`, confirme que `fieldDetails.Health.address` est exposé, puis vérifie via le pipe de la cible que le process .NET voit bien la nouvelle valeur.
- `WritePrimitivePath_UpdatesNestedReferencesAndListItems` — écrit réellement `Self.Health` puis `Inventory.Items[0].Value` depuis l'adresse du `Player`, avec traversal de références et index `List<T>`, vérifie `verified=true`, relit les valeurs et confirme via le pipe de la cible que le process .NET voit bien les mutations.
- `WritePrimitivePath_UpdatesStringReferenceStructAndDictionaryValues` — écrit une `string` managée en place quand la nouvelle valeur a exactement la même longueur, écrit un champ de struct imbriquée (`Stats.Rank`), modifie une valeur interne de `Dictionary<string,int>` (`Inventory.Currencies[gold]`) et remplace une référence d'objet dans un tableau (`Inventory.QuickSlots[2] = 0x...`). Le test vérifie chaque mutation via le pipe de la cible .NET, pas seulement via une relecture ClrMD.
- `WritePrimitivePathBatch_RollsBackAlreadyAppliedWritesOnFailure` — exécute une transaction multi-champs best-effort : si une opération échoue après des écritures déjà appliquées, le helper relit les anciennes valeurs avant mutation et tente un rollback en ordre inverse. Le test provoque volontairement une erreur sur un second chemin et vérifie que `Health` revient à sa valeur de départ côté cible.
- `FieldValueLocator_RefindsObjectAfterCompactingGc` — retrouve `Player` via `type + Name == "TestSubject"`, force un GC Gen2 compactant réel, flush le cache ClrMD, rejoue le locator et relit l'objet retrouvé. Garde-fou direct contre l'erreur classique "adresse CLR stockée comme si elle était stable".
- `FieldValueLocator_FindsPrimitiveFieldMatches` — retrouve un `Item` par champ primitif (`Value == 150`) et vérifie que le résultat porte `identityField`/`identityValue`, pas seulement une adresse brute.

Les tests lancent de **vrais processus** (pas de mock) via `ManagedProcessFixture`/`PipeClient` (C#, pas d'appel à `powershell.exe` depuis les tests — plus rapide et plus fiable en exécution automatisée), et nettoient proprement (`shutdown` RPC puis `Kill` si nécessaire) même si un test échoue en cours de route. Aucun process ni fichier marqueur orphelin constaté après une exécution complète (le kill abrupt laisse volontairement un marqueur derrière lui — conséquence réaliste du scénario testé — nettoyé en best-effort par le test lui-même). `BuiltAssemblyLocator` retrouve les DLL construites en remontant depuis le dossier de sortie des tests jusqu'à la racine du dépôt (marqueur `AGENTS.md`), Release ou Debug selon ce qui est disponible — pas de chemin absolu figé.

Toute la suite tourne dans une seule collection xUnit (`ClrInspectorEndToEnd`, `DisableParallelization = true`) : les noms de pipe étant des constantes/env vars, deux tests qui écouteraient sous le même nom en parallèle se partageraient les connexions de façon non déterministe — la désactivation de la parallélisation est donc une condition de correction, pas juste une prudence.

## Limitation connue, non bloquante pour ce MVP

**Le root `StaticVar` (`TestRoot.RootPlayer`, champ static) n'apparaît pas dans `heap.EnumerateRoots()` en attache passive live** — seul le root `StrongHandle` (`GCHandle` explicite) et des roots `Stack` transitoires (variables locales du thread de churn en cours d'exécution) ont été observés. Hypothèse la plus probable : l'énumération complète des racines statiques sur un process **vivant et non suspendu** est un best-effort côté DAC, potentiellement moins fiable qu'une attache invasive (`suspend:true`) ou qu'une analyse de dump figé. Le critère demandé ("retrouver au moins une GC root") est rempli par le root `StrongHandle`, qui utilise un mécanisme de root réellement différent — donc pas bloquant pour ce MVP — mais à investiguer avant de considérer la couverture des roots comme complète pour l'enrichissement futur (piste : tester `suspend:true` sur `AttachToProcess`, ou `DataTargetOptions.ForceCompleteRuntimeEnumeration`).

## Intégration UI livrée le 21/08/2026

- Backend natif : `ApplicationController` lance `KillEngineClrInspector.exe` au besoin, injecte un nom de pipe isolé par PID KillEngine (`KILLENGINE_CLR_INSPECTOR_PIPE_NAME`) et expose `getClrInspectorStatus`, `attachClrInspector`, `detachClrInspector`, `shutdownClrInspector`, `flushClrInspectorCache`, `findClrObjectsByType`, `readClrObject`, `enumerateClrRoots`.
- Frontend : nouvelle vue `CLR` dans la navigation principale (`ui/src/views/ClrInspectorView.vue`), avec statut helper, attache CLR sur le process courant, filtre par sous-chaîne de type, liste d'objets, lecture d'adresse manuelle, champs de l'objet sélectionné, aperçu JSON des collections déroulées, roots GC.
- Suite du 22/08/2026 : la vue `CLR` expose aussi un locator par champ (`type`, `champ`, `valeur`, limite de résultats). Le bouton `Locator` sur un champ utilisable préremplit le formulaire, puis `Retrouver` renvoie les objets candidats et permet de relire directement l'adresse actuelle.
- Suite profil du 22/08/2026 : `LocatorKind::ClrField` est sérialisé dans les `.keprofile` (`typeSubstring`, `identityField`, `identityValue`, `targetField`). `ApplicationController::saveClrFieldProfileTarget` valide le locator via ClrMD avant sauvegarde ; `resolveProfileTarget` relocalise l'objet par `findObjectsByFieldValue`; `ProfileView.vue` sauvegarde le champ CLR sélectionné et écrit ensuite via `writePrimitiveField`.
- Suite Assistant/Trainer du 22/08/2026 : `activateProfileTarget` accepte maintenant les cibles CLR en les gardant comme cibles symboliques actives (`type + champ d'identité + champ cible`) ; `writeProfileTargetsFromQuery` relocalise l'objet puis appelle `writeClrPrimitiveField` au lieu de `writeMemoryValueConfirmed`. Côté Trainer, une feature `clr_write` se crée depuis l'inspecteur CLR et rejoue le même locator logique avant écriture. Les champs CLR ne sont pas ajoutés au rollback mémoire brut.
- Suite packaging du 22/08/2026 : le package portable embarque le helper self-contained et valide sa présence comme runtime item requis, sauf `-SkipClrInspector`.
- Suite clôture du 22/08/2026 : `writePrimitivePath` est exposé du helper jusqu'à `ApplicationController::writeClrPrimitivePath`, `backend.ts`, `app.ts` et la vue `CLR`. Le panneau `Écrire chemin` permet de saisir un chemin symbolique borné depuis l'objet lu, par exemple `Self.Health` ou `Inventory.Items[0].Value`, avec confirmation de risque et télémétrie `clr_inspector_write_path`.
- Suite avancée du 22/08/2026 : `writePrimitivePath` couvre aussi les `string` managées de même longueur, les changements de références objet (`0x...`, `0`, `null`), les champs primitifs dans des structs imbriquées, les valeurs primitives internes de `Dictionary<K,V>` par clé (`Inventory.Currencies[gold]`) et les slots de tableaux/`List<T>` de références. `writePrimitivePathBatch` ajoute une transaction multi-champs best-effort avec rollback inverse en cas d'échec, exposée jusqu'à `ApplicationController::writeClrPrimitivePathBatch`, `backend.ts`, `app.ts` et la vue `CLR` via le panneau `Transaction`.
- Le helper reste optionnel à l'exécution : si `KillEngineClrInspector.exe` est volontairement absent d'un package dev, le panneau échoue avec un message clair sans casser KillEngine.

## Extensions futures possibles, non bloquantes

- **Déballage arbitraire de toutes les collections** — le déballage livré est borné et pragmatique (`List<T>`, tableaux 1D, `Dictionary<K,V>`, plus collections custom simples adossées à un tableau privé `_items`/`items` et un compteur `_size`/`_count`, 32 éléments max). Il ne prétend pas couvrir tous les types de collection BCL, les tableaux multidimensionnels, les itérateurs à logique métier ni un graphe récursif profond.
- **Mutation avancée via ClrMD** — le socle livré couvre désormais les champs primitifs directs, les chemins de références/tableaux/List<T>, les `string` même longueur, les références objet, les structs imbriquées, les valeurs internes de `Dictionary<K,V>` et une transaction multi-champs best-effort. Restent hors de ce lot : propriétés avec logique setter (chantier traité séparément par Claude), tableaux primitifs/structs en écriture directe par index, mutation par chemin symbolique relocalisé automatiquement après GC et vraie transaction atomique avec suspension coordonnée du runtime.
- **Génération de rapport / résolution de type imbriqué récursif profond, désassemblage de méthodes, GCRoot chain complet (chemin racine→objet, pas juste "un root existe")** — tout ce qui dépasse les 7 points explicitement demandés pour le MVP.

## Liens

- `docs/POWER_UP_ROADMAP.md` candidat #8 — statut à jour, contexte produit.
- `docs/KILLENGINE_CLR_TEST_TARGET_SPEC.md` — la cible de test sur laquelle ce helper a été développé et validé.
- `docs/STRATEGY_ROOM.md` — raisonnement complet (investigation Solitaire, vérification candidat #8, décision de construire ce MVP).
- `apps/desktop/automation_pipe_server.h/.cpp` — protocole JSON-RPC repris à l'identique.
