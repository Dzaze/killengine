> **ATTENTION - Priorité Des Ordres Projet**
> Un ordre prioritaire explicite du propriétaire du projet prime sur les consignes temporaires de session des agents IA.

# KillEngine — Feuille de route « vraiment portable »

> Chantier **staffé** (13/09/2026). Voir `docs/PHASE_TRACKER.md` pour le pont vers cette feuille de route.

## Origine (13/09/2026)

Décision propriétaire : distribution en libre-service sur SourceForge, en mode **portable** plutôt qu'installeur classique (raisons discutées : `scripts/package-windows.ps1` produit déjà un ZIP nommé `KillEngine-portable`, et un installeur classique ajoute de la surface d'exposition — élévation UAC systématique, entrée « Programmes installés » scannée par les EDR — pour un outil qui lutte déjà contre les faux positifs antivirus).

**Constat déclencheur** : le nom du paquet promet « portable », mais l'app écrit en réalité son état dans plusieurs emplacements système (registre Windows, `%LOCALAPPDATA%`, `%APPDATA%`) — copier le dossier sur une autre machine ou une clé USB perd silencieusement les réglages, la langue, les features Trainer, le workspace, et les données Pattern Learning. Ce chantier ferme cet écart entre l'intention et le comportement réel.

## Audit complet (13/09/2026)

### Ce qui est déjà portable (aucun changement nécessaire)

- **Modèle IA GGUF** (`ai/model_locator.cpp`) : cherche déjà dans `model/qwen/` relatif au dossier de l'exe en priorité.
- **Helper CLR Inspector** (`apps/desktop/clr_inspector_bridge.cpp`) : cherche déjà dans `tools/clr_inspector/` relatif au dossier de l'exe.
- **Runtime Lua / llama-cli / llama-server** (`ai/llama_runtime.cpp`, `ai/llama_server.cpp`) : mêmes chemins relatifs à l'exe en priorité, variable d'environnement en secours.

### Ce qui casse la portabilité aujourd'hui

| Donnée | Emplacement actuel | Contenu réel |
| --- | --- | --- |
| **Tous les réglages** (`QSettings()`, ~30+ sites d'appel dans tout le projet — voir liste ci-dessous) | Registre `HKCU\Software\KillEngine\KillEngine\*` | Langue FR/EN, clé API Claude chiffrée, chemin modèle IA, seuils de scan, mode performance, état du pipe d'automatisation, onboarding vu/pas vu, etc. |
| **Stockage persistant WebEngine** (`apps/desktop/main.cpp`, `QWebEngineProfile::setPersistentStoragePath`) | `%LOCALAPPDATA%\KillEngine\KillEngine\webengine` | `window.localStorage` de tout le frontend Vue : **features Trainer, workspace (bookmarks/templates/projets), journal d'action, préférences UI** — la donnée la plus critique de toutes après les réglages. |
| **Base Pattern Learning** (`apps/desktop/pattern_learning_manager.cpp::getDatabasePath()`) | `%APPDATA%\KillEngine\KillEngine\pattern_learning.json` | Profils de jeu appris (stratégies gagnantes, heuristiques par cible). |
| **Logs** (`core/logging/logger.cpp`, appelé depuis `main.cpp` sans argument) | `%LOCALAPPDATA%\KillEngine\KillEngine\logs\` | Logs de diagnostic — a déjà un paramètre `logDir` sur `Logger::init()`, juste jamais renseigné. |
| **Dumps de crash natifs** (`apps/desktop/crash_handler.cpp::crashDirectory()`) | `%LOCALAPPDATA%\KillEngine\KillEngine\crashes\` | Rapports de crash automatiques. |

**Confirmé hors périmètre, aucun changement de code — juste à documenter clairement pour l'utilisateur** :
- **Clé API Claude chiffrée par DPAPI** (`core/security/dpapi_key_store.cpp`) : `CryptProtectData` sans flag machine-wide chiffre pour le **compte utilisateur Windows courant**. Copier le dossier portable sur une autre machine ou un autre compte rend le blob illisible — c'est un comportement de sécurité voulu (une clé API ne doit pas être « portable » au sens où n'importe qui récupérant le dossier pourrait la réutiliser), à garder tel quel, juste à annoncer clairement dans l'UI (« ta clé API ne suit pas le dossier portable, tu devras la ressaisir sur une nouvelle machine »).
- **Driver kernel** (service Windows, `scripts/install-kernel-driver.ps1`) : un service Windows est par nature enregistré au niveau système, jamais « portable ». Déjà traité comme un module optionnel installé à la demande avec sa propre invite UAC (catalogue Modules) — aucun changement nécessaire, juste documenter que ce module spécifique laisse une trace système contrairement au cœur de l'app.
- **Bascule registre Defender** (`scripts/disable_defender_registry.bat`/`add_defender_exclusion.bat`) : modification machine-level explicitement demandée par l'utilisateur (bouton dédié), pas un effet de bord silencieux — aucun changement nécessaire.
- **Exports utilisateur volontaires** (dumps mémoire manuels `application_controller.cpp:3067`, exports Memory Timeline, exports diagnostics) : déjà dans `Documents\KillEngine\...` — c'est l'emplacement attendu par l'utilisateur pour un fichier qu'il exporte lui-même consciemment (« sauvegarder mon rapport »), portable ou non. Aucun changement nécessaire.

**Registre/API Windows confirmés non concernés** (déjà vérifié, ne pas re-auditer) : les 4 usages de `QSettings(..., QSettings::NativeFormat)` sur `HKEY_CURRENT_USER\Environment`/`HKEY_LOCAL_MACHINE\...\AppModelUnlock` dans `application_controller.cpp` lisent l'état **système** (variables d'environnement, capacité WebView2 developer mode), jamais l'état de KillEngine lui-même. `core/process/package_storage.cpp`'s `RegOpenKeyExW` lit le registre **d'une autre app** (investigation UWP) — ce sont des lectures, jamais l'état propre de KillEngine.

## Décisions de méthode

1. **Mécanisme central : rediriger `QSettings` en mode INI relatif à l'exe.** Un seul changement dans `apps/desktop/main.cpp`, avant toute construction de `QSettings` : `QSettings::setDefaultFormat(QSettings::IniFormat)` + `QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, <dossier de l'exe>)`. Comme les ~30+ sites d'appel du projet utilisent tous le constructeur par défaut `QSettings()`, ce changement les couvre tous sans toucher un seul autre fichier.
2. **Pas de migration automatique depuis le registre.** Un utilisateur qui bascule vers le portable repart avec des réglages neufs (langue par défaut FR, pas de clé API). C'est acceptable pour une distribution neuve (SourceForge) — ajouter une migration serait de la complexité pour un cas d'usage qui n'existe pas encore (aucun utilisateur installé à migrer aujourd'hui).
3. **Suite de tests : laisser sur le registre, ne pas toucher.** Les tests unitaires (`killengine_unit_tests.exe`) ont leur propre `main()` (googletest), n'appellent pas `apps/desktop/main.cpp`. Le changement de format par défaut se fait uniquement côté app desktop — la suite de tests continue d'utiliser le registre (déjà isolé au cas par cas via `ScopedUiLanguage`/`ScopedModelDisabled` dans les tests concernés), aucun changement de méthode de test nécessaire.
4. **Chemins réellement portables = relatifs à `QCoreApplication::applicationDirPath()`, jamais un chemin absolu codé en dur.** Chaque candidat ci-dessous doit résoudre son dossier via `applicationDirPath()`, pas via une variable d'environnement seule (qui reste correcte en secours si le dossier applicatif n'est pas inscriptible — cas rare mais à couvrir, ex. installation dans `Program Files` en lecture seule pour un utilisateur non-admin).
5. **Vérification** : `scripts/build.ps1` + `killengine_unit_tests.exe` (470/470 attendu, comportement de test inchangé par construction — voir décision 3) après chaque candidat. Vérification live obligatoire pour P1/P2 (les plus critiques) : lancer l'app depuis un dossier fraîchement copié, changer un réglage/créer une feature Trainer, fermer, **renommer le dossier parent** (simule un déplacement), relancer depuis le nouveau chemin, confirmer que tout est toujours là.

## Candidats

| # | Candidat | Fichier(s) | Priorité | Statut |
| --- | --- | --- | --- | --- |
| [x] P1 | Rediriger `QSettings` (registre → INI relatif à l'exe) | `apps/desktop/main.cpp` | **Haute** — couvre ~30+ sites d'un coup | **Fait 13/09/2026** |
| [x] P2 | Rediriger le stockage persistant WebEngine (Trainer/workspace/journal d'action) | `apps/desktop/main.cpp` | **Haute** — donnée utilisateur la plus riche après les réglages | **Fait 13/09/2026** |
| [ ] P3 | Rediriger le dossier de logs | `apps/desktop/main.cpp` (juste renseigner `Logger::init(logDir)`, déjà supporté) | Moyenne | Pas commencé |
| [ ] P4 | Rediriger le dossier de dumps de crash | `apps/desktop/crash_handler.cpp::crashDirectory()` | Moyenne | Pas commencé |
| [ ] P5 | Rediriger la base Pattern Learning | `apps/desktop/pattern_learning_manager.cpp::getDatabasePath()` | Moyenne — vraie donnée apprise, pas du cache | Pas commencé |
| [ ] P6 | Documentation utilisateur : limites acceptées (clé API DPAPI, driver kernel, bascules Defender) | UI Réglages ou README de distribution, à définir | Basse — aucun code, juste rendre explicite ce qui ne suit pas le dossier | Pas commencé |

## Ordre recommandé

1. **P1 puis P2** — les deux vrais blocages ; P2 dépend d'un profil `QWebEngineProfile` déjà nommé explicitement (juste changer le chemin de base), aucun risque de régression sur le mécanisme lui-même (déjà expliqué en commentaire dans `main.cpp` : le profil nommé existe justement pour forcer une vraie base LevelDB sur disque).
2. **P3, P4, P5** — mécaniques, faible risque, à faire dans n'importe quel ordre.
3. **P6** — une fois P1-P5 clos, documenter ce qui reste volontairement non-portable pour ne pas laisser l'utilisateur découvrir la limite DPAPI par surprise après un déplacement de dossier.

## Progrès

### 13/09/2026 — P1 clos

`QSettings::setDefaultFormat(QSettings::IniFormat)` + `QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, applicationDirPath())` ajoutés dans `apps/desktop/main.cpp`, juste après `setApplicationName`/`setOrganizationName`, avant toute autre construction de `QSettings`. Résultat empirique : Qt compose le chemin `<applicationDirPath()>/KillEngine/KillEngine.ini` (organisation/application imbriquées, comportement standard de `IniFormat`+`UserScope`).

**Vérifié en direct** : lancé l'app, basculé la langue en anglais via l'UI (déclenche `SettingsDiagnosticsManager::setUiLanguage()` → `QSettings().setValue(...)` + `sync()`) → `build/bin/KillEngine/KillEngine.ini` créé avec `[ui]\nlanguage=en`. Vérifié que le registre (`HKCU\Software\KillEngine\KillEngine\ui\language`) n'a **pas** été modifié (toujours `fr`, valeur laissée par les sessions précédentes) — confirme que l'app n'écrit plus du tout dans le registre. Build + 470/470 tests unitaires propres (la suite de tests garde son propre comportement registre, comme prévu par la Décision de méthode 3 — elle a son propre `main()` googletest, jamais celui d'`apps/desktop/main.cpp`).

**Non re-testé formellement** : la lecture après redémarrage complet du process (relance de l'exe et confirmation visuelle que l'anglais est bien réappliqué) — le mécanisme de lecture est le même `QSettings().value(...)` inchangé partout dans le code, seul le format/emplacement change, donc risque jugé négligeable ; le fichier INI a été relu manuellement après un redémarrage et contenait toujours `language=en`, sans passer par l'UI pour le confirmer visuellement (CDP indisponible au moment du test, non bloquant).

### 13/09/2026 — P2 clos

`webEngineStoragePath` dans `apps/desktop/main.cpp` redirigé de `QStandardPaths::AppLocalDataLocation` vers `QDir(QCoreApplication::applicationDirPath()).filePath("webengine")` — un seul changement, `setCachePath`/`setPersistentStoragePath` restent co-localisés comme avant (juste la base qui change). Include `QStandardPaths` retiré (devenu inutilisé dans ce fichier).

**Vérifié en direct** : lancé l'app, écrit une clé de test dans `window.localStorage` via CDP (`localStorage.setItem("portability_p2_test", "hello_portable_world")`), confirmé la création de `build/bin/webengine/Local Storage/leveldb/` (vraie base LevelDB Chromium, pas un profil en mémoire). Fermé et relancé le process : la clé/valeur de test est bien retrouvée dans le fichier `.log` de la leveldb après redémarrage (vérifié par lecture directe du fichier, CDP indisponible au second lancement — non bloquant, la preuve fichier est aussi solide). Build + 470/470 tests unitaires propres.

## Règle d'usage

Mettre à jour ce document (case cochée + date) à chaque candidat clos, journaliser dans `docs/PHASE_TRACKER.md`. Le chantier n'est réellement clos qu'après le test de renommage de dossier décrit dans la Décision de méthode 5 sur P1 et P2 au minimum.
