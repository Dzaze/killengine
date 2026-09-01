# Spec - Inspecteur UWP State

**Statut** : Draft v0.1  
**Auteur** : Cline (Kimi K2.5)  
**Date** : 2026-09-01  
**Lane** : UWP-STATE-SPEC (SALON.md)  

---

## Probleme

KillEngine ne sait pas identifier quel fichier UWP change quand une valeur affichee (XP, score, progression) change. Sur Solitaire XP, la vraie source de donnees n'est ni en memoire classique (scan Int32/Float32 vides), ni dans les WebView2/CDP (targets generiques sans gameplay). La piste la plus prometteuse est le stockage fichier UWP (`LocalState`, `LocalSettings`), mais il n'existe aucun outil pour :

1. Lister automatiquement les dossiers de stockage d'un package UWP
2. Capturer un snapshot des fichiers (timestamps, tailles, hash)
3. Comparer deux snapshots pour isoler les fichiers modifies
4. Guider l'utilisateur vers le fichier pertinent sans fouille manuelle

---

## Objectif produit

Ajouter un panneau **"UWP State Inspector"** dans la vue Investigation qui permette de :
- Decouvrir automatiquement les chemins `LocalState`, `RoamingState`, `TempState`, `Settings` d'un process UWP attache
- Prendre des snapshots avant/apres une action utilisateur (ex: gagner de l'XP)
- Visualiser le diff (fichiers ajoutes, supprimes, modifies avec delta de taille)
- Identifier les candidats les plus probables pour l'etape suivante (Save File Value Radar)

---

## Technologie proposee

### Backend (C++ / Qt)

| Composant | Techno | Raison |
|-----------|--------|--------|
| Enumeration packages UWP | `Windows.Management.Deployment.PackageManager` (WinRT) ou parsing `HKCU\Software\Classes\Local Settings\Software\Microsoft\Windows\CurrentVersion\AppContainer\Mappings` | Identifier le package associe au PID |
| Resolution chemins | `Windows.Storage.ApplicationData` (WinRT) ou chemins connus (`%LOCALAPPDATA%\Packages\<PackageFamilyName>\LocalState`) | Localiser LocalState/RoamingState/TempState/Settings |
| Snapshot fichiers | `QDirIterator` + `QFileInfo` (timestamps, taille) + `QCryptographicHash::Sha1` (hash rapide) | Qt deja disponible, pas de dependance externe |
| Stockage snapshot | `QJsonDocument` (metadata) + fichiers binaires references (optionnel) | Format lisible, extensible |
| Diff | Comparaison hash + taille + mtime | Detection rapide des changements |

### Frontend (Vue 3 / TypeScript)

- Panneau dedie dans `InvestigationView` ou onglet supplementaire
- Arbre de fichiers avec etats : `unchanged` | `added` | `removed` | `modified`
- Boutons : `Snapshot Avant`, `Snapshot Apres`, `Comparer`, `Exporter`
- Filtres : par dossier (LocalState/RoamingState/TempState/Settings), par extension, par taille

---

## Architecture proposee

```
┌─────────────────────────────────────────────────────────────┐
│  UwpStateInspector (core/uwp/uwp_state_inspector.h/.cpp)   │
│  ├─ resolvePackageFromProcess(pid) -> PackageInfo            │
│  ├─ getStoragePaths(packageFamilyName) -> StoragePaths       │
│  ├─ takeSnapshot(path, options) -> Snapshot                  │
│  ├─ compareSnapshots(before, after) -> DiffResult          │
│  └─ exportSnapshot(snapshot, path) -> bool                   │
├─────────────────────────────────────────────────────────────┤
│  ApplicationController (Q_INVOKABLE)                         │
│  ├─ getUwpStoragePaths() -> QVariantMap                      │
│  ├─ takeUwpSnapshot(pathKey) -> QString (snapshotId)       │
│  ├─ compareUwpSnapshots(id1, id2) -> QVariantList          │
│  └─ exportUwpSnapshot(id, filePath) -> bool                │
├─────────────────────────────────────────────────────────────┤
│  UI (InvestigationView.vue)                                │
│  ├─ Panneau "UWP State"                                     │
│  ├─ Liste des dossiers detectes                           │
│  ├─ Boutons snapshot + comparaison                          │
│  └─ Tableau de diff avec filtres                            │
└─────────────────────────────────────────────────────────────┘
```

---

## Prototype minimal

### Phase 1 : Spec + validation (ce document)
- [ ] Valider l'approche avec le proprietaire
- [ ] Confirmer que Qt (`QDir`, `QFileInfo`, `QCryptographicHash`) suffit
- [ ] Identifier si WinRT est necessaire ou si les chemins connus suffisent

### Phase 2 : Backend core (C++)
- [ ] `UwpStateInspector::resolvePackageFromProcess()` - mapping PID -> PackageFamilyName
- [ ] `UwpStateInspector::getStoragePaths()` - resolution des 4 chemins standard
- [ ] `UwpStateInspector::takeSnapshot()` - recursion + hash SHA1
- [ ] `UwpStateInspector::compareSnapshots()` - diff algorithmique

### Phase 3 : Bridge ApplicationController
- [ ] Methodes Q_INVOKABLE exposees
- [ ] Tests unitaires avec mock filesystem (Qt temp dir)

### Phase 4 : UI Vue
- [ ] Composant `UwpStatePanel.vue`
- [ ] Integration dans `InvestigationView.vue`
- [ ] i18n (fr/en)

### Phase 5 : Validation terrain
- [ ] Test sur Solitaire : snapshot avant partie, gagner XP, snapshot apres
- [ ] Verifier qu'au moins un fichier change (probablement dans `LocalState`)
- [ ] Documenter le format trouve pour la phase Save File Value Radar

---

## Risques et limites

| Risque | Impact | Mitigation |
|--------|--------|------------|
| Chemins `LocalState` verrouilles pendant execution | Snapshot partiel ou echec | Retry avec delai, ou copie shadow si necessaire (VSS) |
| Packages UWP sans chemins standards | Non detection | Fallback : scanner `%LOCALAPPDATA%\Packages\*` par pattern |
| Performance sur gros dossiers (>10k fichiers) | UI freeze | Pagination async, limite de profondeur configurable |
| Hash SHA1 lent sur fichiers >100MB | Latence | Limite de taille pour hash (ex: hash des 4KB premiers + taille) |
| WinRT non disponible sur certaines versions Windows | Echec resolution package | Fallback registre uniquement |

---

## Validation proposee

### Test unitaire (C++)
```cpp
TEST(UwpStateInspector, TakesSnapshotOfTempDir) {
    QTemporaryDir tempDir;
    createDummyFiles(tempDir.path(), {"a.txt", "b/c.txt"});
    
    auto snapshot = inspector.takeSnapshot(tempDir.path());
    EXPECT_EQ(snapshot.fileCount, 2);
    EXPECT_FALSE(snapshot.files[0].hash.isEmpty());
}

TEST(UwpStateInspector, DetectsModifiedFile) {
    auto before = inspector.takeSnapshot(path);
    modifyFile(path + "/a.txt");
    auto after = inspector.takeSnapshot(path);
    
    auto diff = inspector.compareSnapshots(before, after);
    EXPECT_EQ(diff.modified.size(), 1);
}
```

### Test integration (live)
1. Attacher Solitaire
2. Ouvrir panneau UWP State
3. `Snapshot Avant` sur LocalState
4. Jouer une partie, gagner de l'XP
5. `Snapshot Apres`
6. `Comparer` -> attendre au moins 1 fichier modifie
7. Noter le chemin et l'heure de modification

---

## Fichiers a creer/modifier

### Nouveaux fichiers
- `core/uwp/uwp_state_inspector.h`
- `core/uwp/uwp_state_inspector.cpp`
- `ui/src/components/investigation/UwpStatePanel.vue` (ou integration directe)
- `tests/unit/test_uwp_state_inspector.cpp`

### Fichiers modifies
- `core/CMakeLists.txt` - ajout du nouveau module
- `apps/desktop/application_controller.h/.cpp` - methodes Q_INVOKABLE
- `ui/src/services/backend.ts` - interface TypeScript
- `ui/src/views/InvestigationView.vue` - integration UI
- `ui/src/i18n/fr.json` + `en.json` - traductions

---

## Dependances

- **Aucune nouvelle dependance externe**
- Qt 6.x deja present (`QDir`, `QFileInfo`, `QCryptographicHash`, `QJsonDocument`)
- WinRT optionnel (fallback registre disponible)

---

## Notes de conception

### Pourquoi pas de diff binaire profond dans cette phase ?
Le diff structurel de fichiers binaires (idee #4 du point de controle Solitaire XP) est volontairement **hors scope** de ce premier spec. L'objectif ici est uniquement d'**identifier quel fichier a change**, pas de comprendre comment. Le Save File Value Radar viendra ensuite pour chercher la valeur dans le fichier identifie.

### Pourquoi SHA1 et pas CRC32/MD5 ?
- SHA1 est disponible nativement dans Qt (`QCryptographicHash::Sha1`)
- Assez rapide pour des fichiers <10MB
- Moins de collisions que CRC32 pour la detection de changement

### Pourquoi pas de compression des snapshots ?
Premiere version : stockage JSON metadata uniquement (chemins, tailles, hash, timestamps). Les fichiers binaires eux-memes ne sont pas copies sauf option explicite "Exporter". Cela evite la consommation disque excessive.

---

## Prochaines etapes apres ce spec

1. **Validation par le proprietaire** de l'approche
2. **Decision** : implementer maintenant ou attendre que WEBVIEW-F soit stabilise ?
3. Si go : creer la tache dans PHASE_TRACKER.md et commencer Phase 2 (backend)

---

## References

- `docs/SALON.md` - Point de controle Solitaire XP, idee #2
- `docs/PHASE_TRACKER.md` - PHASES 257-270 (investigation terrain Solitaire)
- Precedent "Bulles" - package UWP avec stockage fichier
- `docs/KILLENGINE_CODE_MAP.md` - structure des dossiers core/ui
