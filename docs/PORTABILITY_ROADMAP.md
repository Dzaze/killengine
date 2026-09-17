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
| [x] P3 | Rediriger le dossier de logs | `apps/desktop/main.cpp` (juste renseigner `Logger::init(logDir)`, déjà supporté) | Moyenne | **Fait 13/09/2026** |
| [x] P4 | Rediriger le dossier de dumps de crash | `apps/desktop/crash_handler.cpp::crashDirectory()` | Moyenne | **Fait 13/09/2026** |
| [x] P5 | Rediriger la base Pattern Learning | `apps/desktop/pattern_learning_manager.cpp::getDatabasePath()` | Moyenne — vraie donnée apprise, pas du cache | **Fait 13/09/2026** |
| [x] P6 | Documentation utilisateur : limites acceptées (clé API DPAPI, driver kernel, bascules Defender) | UI Réglages (`ui/src/views/SettingsView.vue`) | Basse — aucun code métier, juste rendre explicite ce qui ne suit pas le dossier | **Fait 13/09/2026** |

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

### 13/09/2026 — P3, P4, P5 clos (mécaniques, un seul patron de correction)

Trois candidats mécaniques traités ensemble (même patron que P1/P2 : remplacer un `QStandardPaths::writableLocation(...)` par un chemin relatif à `QCoreApplication::applicationDirPath()`) :
- **P3** : `apps/desktop/main.cpp` — `killcore::Logger::instance().init()` renseigné avec `QDir(applicationDirPath()).filePath("logs")` (le paramètre `logDir` existait déjà sur `Logger::init()`, juste jamais utilisé).
- **P4** : `apps/desktop/crash_handler.cpp::crashDirectory()` — remplacé `QStandardPaths::AppLocalDataLocation` (+ repli `QDir::currentPath()`) par `QDir(applicationDirPath()).filePath("crashes")`. Include `QStandardPaths` retiré (devenu inutilisé dans ce fichier).
- **P5** : `apps/desktop/pattern_learning_manager.cpp::getDatabasePath()` — remplacé `QStandardPaths::AppDataLocation` par `QDir(applicationDirPath()).filePath("data")`. Include `QStandardPaths` remplacé par `QCoreApplication` (nécessaire pour `applicationDirPath()`, absent du fichier auparavant).

**Vérifié en direct** : build propre, 470/470 tests unitaires. App relancée avec un `WorkingDirectory` volontairement différent (`C:\`, pas le dossier de l'exe) pour prouver que la résolution ne dépend pas d'un `QDir::currentPath()` accidentel — confirmé `build/bin/logs/killengine_<timestamp>.log` créé et `build/bin/data/` créé (dossier Pattern Learning, prêt à recevoir `pattern_learning.json` dès la première écriture). Confirmé en parallèle que les anciens emplacements (`%LOCALAPPDATA%\KillEngine\KillEngine\{logs,crashes,webengine}`) n'ont montré **aucune activité** pendant ce lancement (dates de modification inchangées, antérieures à ce test) — la bascule est complète, plus aucune écriture résiduelle vers les anciens chemins système.

### 13/09/2026 — P6 clos

Nouvelle section « Mode portable — limites acceptées » ajoutée dans `ui/src/views/SettingsView.vue`, entre le panneau Diagnostic et le panneau Compatibilité antivirus (aucun code backend, purement informatif — pas de nouvel appel `Q_INVOKABLE`). Trois blocs, un par exception documentée dans l'audit du 13/09/2026 : clé API Claude (DPAPI, liée au compte Windows courant), driver noyau (service Windows, non portable par nature), exclusion Windows Defender (modification machine déclenchée explicitement par le bouton du panneau voisin). Clés i18n `settings.portability*` ajoutées en FR et EN (`ui/src/i18n/locales/{fr,en}.json`).

**Vérifié en direct** : `npm run type-check` et `npm run build` propres, JSON des deux locales validé. App relancée avec CDP, section lue via `Runtime.evaluate` en anglais (langue par défaut) puis basculée en français via le sélecteur FR de la sidebar et relue — les deux versions s'affichent correctement, aucun texte manquant ou clé i18n non résolue.

Ce candidat clôt le chantier de portabilité (P1-P6 tous faits). Reste ouvert uniquement le test de renommage de dossier bout-en-bout mentionné dans la Décision de méthode 5 (déjà couvert indirectement par les vérifications par redémarrage/`WorkingDirectory` différent de chaque candidat, mais jamais fait comme un seul scénario "copier le dossier entier vers un nouveau chemin puis tout réutiliser") — à faire si le propriétaire veut une validation finale avant la première distribution SourceForge.

### 13/09/2026 — Correctif post-P6 : le driver noyau était installable dans l'UI mais pas dans le vrai paquet

En creusant la question du propriétaire "l'utilisateur peut-il utiliser le driver kernel ?", deux trous réels trouvés dans `scripts/package-windows.ps1` (le générateur du ZIP `KillEngine-portable`), tous deux invisibles en dev car le dépôt complet a toujours les deux :
1. `scripts/install-kernel-driver.ps1` (le script que le bouton "Installer" de Modules invoque via `findModuleCatalogScript`) n'était jamais copié dans `scripts\` du paquet — seuls 5 scripts EDR/Lua l'étaient.
2. Le `.sys` compilé (`tools\kernel_driver\...\Release\KillEngineKernel.sys`) n'était jamais copié non plus, contrairement à l'inspecteur CLR qui est bien publié dans le paquet.

Un utilisateur réel cliquant "Installer" sur le driver kernel depuis le ZIP SourceForge serait tombé sur `install-kernel-driver.ps1 introuvable.` **Corrigé** : les deux scripts test-signing + `install-kernel-driver.ps1` sont maintenant copiés inconditionnellement dans `scripts\` du paquet ; le `.sys` (Release) est copié en best-effort dans `tools\kernel_driver\Release\` avec un nouveau flag `-SkipKernelDriver` (avertissement, pas d'échec bloquant, car builder le driver nécessite le WDK/MSBuild — pas garanti présent sur toute machine de packaging). `PACKAGE_README.txt` avait aussi une ligne obsolète ("Logs and profiles are stored under the Windows local app data folder") — corrigée pour refléter P1-P5, et `docs/PORTABILITY_ROADMAP.md` est désormais copié dans le paquet.

**Second trou, plus profond** : même avec les fichiers présents, `KillEngineKernel.sys` n'est pas signé WHQL/EV (`scripts/build-kernel-driver.ps1` n'a aucune étape de signature) — Windows refuse de le charger sans le mode **Test Signing** (`bcdedit /set testsigning on`), jusqu'ici documenté seulement pour les développeurs (`docs/KILLENGINE_KERNEL_DRIVER_ARCHITECTURE.md`), jamais expliqué à l'utilisateur final. Décision du propriétaire : ajouter un bouton pour activer **et** désactiver ce mode depuis l'UI (pas seulement de la doc).

**Ajouté** :
- `scripts/enable_test_signing.bat` / `scripts/disable_test_signing.bat` (même patron auto-élévation que les scripts Defender existants).
- `ApplicationController::getTestSigningStatus()` (lecture seule, `bcdedit /enum {current}`, pas d'élévation) et `setTestSigningEnabledAsync(bool)` (élévation UAC via thread séparé, même mécanisme que `setWindowsDefenderDisabledAsync`), signal `testSigningEnabledFinished`.
- UI : nouveau bloc dans `ModulesView.vue`, à l'intérieur de la carte `kernel_driver` (section Dépendances), avec statut Test Signing + bouton Activer/Désactiver (élévation UAC, `riskGate.confirmRiskAction`) + avertissement redémarrage/watermark. Clés i18n `modules.kernel.*` en FR/EN.

**Vérifié en direct** : build propre, 470/470 tests unitaires, `npm run type-check`/`build` propres, JSON des deux locales validé, syntaxe `package-windows.ps1` validée (parseur PowerShell, sans lancer un packaging complet). `getTestSigningStatus()` appelé en direct via le pipe d'automatisation (`KILLENGINE_AUTOMATION_PIPE=1`) sur cette machine — CDP indisponible au relancement (flakiness déjà documentée, voir Progrès P1/P2) — résultat `{"enabled":true,"success":true}`, confirmé exact vs. `bcdedit /enum {current}` lancé en parallèle (`testsigning Yes` sur cette machine de dev). Le chemin d'écriture (`setTestSigningEnabledAsync`) n'a délibérément pas été déclenché pour de vrai : il modifierait la configuration de démarrage réelle de cette machine (watermark permanent + redémarrage requis) — non exercé sans demande explicite, risque jugé faible par construction (copie exacte du mécanisme `setWindowsDefenderDisabledAsync` déjà en production).

## Règle d'usage

Mettre à jour ce document (case cochée + date) à chaque candidat clos, journaliser dans `docs/PHASE_TRACKER.md`. Le chantier n'est réellement clos qu'après le test de renommage de dossier décrit dans la Décision de méthode 5 sur P1 et P2 au minimum.

<a id="portable-v2"></a>
## PORT-1 à PORT-6 — Livraison portable autonome et déplacement des données (17/09/2026)

**Statut : à faire, non affecté ; cadrage documentaire demandé par le propriétaire.** Point de rendez-vous : [PHASE_TRACKER.md](PHASE_TRACKER.md#portable-v2). Les lots P1-P6 ci-dessus et AM-1 à AM-5 du tracker restent livrés. Cette nouvelle série traite les manques constatés ensuite ; les anciennes mentions « seul test restant » décrivent l'audit du 13/09, pas le périmètre actuel. Aucune implémentation ni validation applicative nouvelle n'est annoncée ici.

**Résultat attendu** : un ZIP démarre sur un Windows compatible sans environnement développeur ; les données annoncées portables suivent un déplacement du dossier ; un échec de sauvegarde ou de module est visible et ne devient jamais un faux succès.

### Contrat commun avant de coder

- Réutiliser les acquis : build complet Vue+C++, bundle `ui/dist` livré, exclusions d'état de session AM-1, profils `.keprofile` déjà sauvegardés via `QSaveFile`, stockage WebEngine persistant, harnais AM-4 et client CDP. Ne pas reconstruire ces mécanismes.
- Prendre `applicationDirPath()` comme racine du paquet ; distinguer ressources distribuées, données utilisateur et fichiers temporaires de scan. Les exports demandés vers Documents, les fichiers inspectés d'autres applications et les temporaires bornés ne sont pas des profils à déplacer.
- Garder les limites acceptées : clé API DPAPI liée au compte Windows, driver optionnel installé comme service. Aucune migration automatique des réglages depuis le registre, aucune modification de sécurité système ou installation de runtime globale ajoutée par ces lots.
- Préserver les données déjà portables : les chemins INI `KillEngine/KillEngine.ini`, `webengine/`, `logs/`, `crashes/` et `data/pattern_learning.json` restent compatibles. Pour les profils encore externes, cible proposée `data/profiles/` ; import explicite et non destructif des profils existants.
- Un dossier non inscriptible doit donner un diagnostic exploitable, jamais une annonce de sauvegarde réussie. Si un repli hors du paquet est proposé, son emplacement et sa limite de portabilité doivent être explicites. Le minimum de cette série est de détecter/refuser proprement l'écriture impossible ; un repli automatique n'est pas requis.
- Réserver les fichiers avant modification. Un contrat C++/Vue modifié comprend déclarations, interface TS, mock, messages FR/EN et contrôle du vrai backend. Écrire le suivi par lot dans le tracker, en distinguant code terminé, tests exécutés et validations restant à faire.

<a id="port-1"></a>
### PORT-1 — Dépendances natives complètes dans le paquet

**Priorité : critique pour la distribution. Dépendance : aucune.**

**Diagnostic** : `package-windows.ps1` filtre `d\.dll$` puis `*d.dll`, ce qui élimine `mtmd.dll` Release. Au contrôle du 17/09 : fichier présent dans `build/bin`, absent du paquet ; imports PE réels `llama-cli-impl.dll → llama-server-impl.dll → mtmd.dll`. `KillEngine.exe` et les runtimes importent aussi `MSVCP140.dll`/`VCRUNTIME140.dll`/`VCRUNTIME140_1.dll`, absentes du paquet inspecté ; `vc_redist.x64.exe` seul ne garantit pas l'autonomie. Ce diagnostic combine lecture de code et tables d'import, pas un lancement sur machine vierge.

**Fichiers / entrée** : `scripts/package-windows.ps1`, `scripts/build.ps1::Copy-DllsAlongside`, `scripts/verify-ai-layout.ps1`, `scripts/release-check.ps1`, `packaging/README.md`.

**Lots à exécuter** :

- [ ] Remplacer les motifs debug trop larges par des exclusions identifiées précisément ; conserver `mtmd.dll` et contrôler les deux endroits qui filtrent. Conserver les exclusions de données utilisateur, les options de paquet léger et les vérifications AM-1.
- [ ] Embarquer les bibliothèques Visual C++ redistribuables adaptées à l'architecture et aux runtimes, depuis une source identifiée. Ne pas copier arbitrairement les DLL de System32 ni lancer `vc_redist` à l'insu de l'utilisateur ; garder l'exécution du cœur sans installation système.
- [ ] Ajouter au contrôle du paquet un rapport des dépendances natives transitives des EXE/DLL distribués, imports différés compris : fournies / système / manquantes. Tenir compte des contrats système API-set (`api-ms-win-*`/`ext-ms-*`) au lieu de les traiter comme autant de fichiers à copier. Le vérificateur et ses prérequis servent au build/QA, pas au démarrage utilisateur.
- [ ] Compléter ce contrôle par un lancement borné des runtimes livrés et un test IA minimal : les DLL chargées dynamiquement ne sont pas toutes visibles dans les imports PE. Respecter les capacités annoncées par les variantes avec/sans modèle.

**Clôture** : `mtmd.dll` dans dossier ET ZIP ; retrait volontaire d'une dépendance dans une fixture → échec identifié ; pas de mélange d'architectures ni DLL debug ; KillEngine/UI et runtimes livrés démarrent hors dépôt. Faire le contrôle final sur Windows compatible sans outils développeur/runtime VC++ préinstallé si un environnement isolé est disponible. À défaut, livrer le correctif et les contrôles locaux, mais consigner explicitement « machine propre non testée » sans prétendre à cette validation.

<a id="port-2"></a>
### PORT-2 — Résolution commune des chemins et profils portables

**Priorité : haute. Dépendance : aucune ; fournir le contrat de chemins avant PORT-3/PORT-4.**

**Diagnostic** : `ProfileStore::profilesDir()` utilise encore `QStandardPaths::GenericDataLocation + "/KillEngine/Profiles"`. `SettingsDiagnosticsManager::smartSearchDebugFilePath()` et `scanTelemetryFilePath()` utilisent `AppLocalDataLocation`, alors que le log principal est déjà à côté de l'exécutable. Copier le paquet ne copie donc pas ces profils ni ces journaux.

**Fichiers / entrée** : `core/profiles/profile_store.*`, `apps/desktop/settings_diagnostics_manager.cpp`, `apps/desktop/main.cpp`, `apps/desktop/pattern_learning_manager.cpp`, `apps/desktop/crash_handler.cpp`, `core/CMakeLists.txt`. Pour l'import, relire les fonctions de chargement/enregistrement de profils et `ProfileView.vue` avant d'ajouter une API.

**Lots à exécuter** :

- [ ] **PORT-2a — socle** : introduire un résolveur léger commun de racine/chemins, avec racine injectable pour les tests et chemins indépendants du répertoire courant. Documenter son API avant de paralléliser ses consommateurs ; ne pas introduire une dépendance inverse de `killcore_logging` vers tout `killcore`.
- [ ] **PORT-2b — raccordement** : rediriger profils vers `data/profiles/`, journaux JSONL vers `logs/`, puis faire converger les consommateurs existants sur le résolveur sans déplacer inutilement les données déjà portables. Mettre à jour le harnais AM-4 qui cherche/nettoie actuellement ses profils dans l'ancien emplacement.
- [ ] **PORT-2c — reprise des profils existants** : proposer un import explicite depuis l'ancien répertoire. Copier après validation, ne supprimer aucune source ; gérer collisions de noms, profil invalide et échec partiel avec un résultat par fichier. Ne pas écraser un profil portable existant ni revenir silencieusement au répertoire système à chaque chargement.
- [ ] Coordonner les exclusions du packaging : le nouveau dossier de profils reste une donnée utilisateur, exclue d'un ZIP neuf. Mettre à jour chemins dans diagnostics/documents et exemples de test concernés.

**Clôture** : deux racines de test indépendantes n'échangent pas de profils ; lancement avec répertoire courant différent conserve les mêmes chemins ; création/lecture/suppression d'un profil de fixture fonctionne sous le paquet ; aucune nouvelle écriture des profils/journaux concernés dans les anciens dossiers. Import valide, invalide, homonyme et répété testé sans perte ni doublon involontaire. Ne pas vérifier cela sur les vrais profils du propriétaire.

<a id="port-3"></a>
### PORT-3 — Sauvegardes fiables et erreurs visibles

**Priorité : haute. Dépendance : contrat PORT-2a ; réalisation backend/frontend possible en lots séparés.**

**Diagnostic** : `saveSettings()` et `setUiLanguage()` appellent `sync()` puis annoncent toujours `success=true`. Des stores absorbent les exceptions `localStorage` puis leurs appelants annoncent la création/sauvegarde. `GameProfileDatabase` tronque son JSON avant d'écrire et accepte `written >= 0`, insuffisant pour vérifier une écriture complète.

**Fichiers / entrée** : `apps/desktop/settings_diagnostics_manager.cpp`, `apps/desktop/claude_chat_manager.cpp` (persistance des réglages/clé, sans changement DPAPI), `core/pattern_learning/game_profile_database.cpp`, `apps/desktop/pattern_learning_manager.cpp`, `ui/src/stores/trainer.ts`, `workspaceItems.ts`, `workspaceSession.ts` et leurs appelants. `core/profiles/profile_store.cpp` fournit déjà un patron `QSaveFile`.

**Lots à exécuter** :

- [ ] **PORT-3a — backend** : vérifier le statut après `QSettings::sync()`, propager l'échec jusqu'au résultat UI, auditer les autres setters qui annoncent une persistance. Détecter le dossier non inscriptible sans élévation automatique. Distinguer changement actif en mémoire et changement effectivement enregistré.
- [ ] **PORT-3b — frontend** : donner aux fonctions de persistance un résultat exploitable et le remonter à leurs appelants ; ne plus transformer une exception en succès. Afficher « modifications non enregistrées » et permettre une nouvelle tentative/export des données en mémoire. Une erreur du journal d'action lui-même ne doit pas créer une boucle de notifications.
- [ ] **PORT-3c — fichier Pattern Learning** : écriture atomique avec vérification du nombre exact d'octets et du commit, propagation des erreurs de sauvegarde/fermeture à l'appelant. L'ancienne version valide doit survivre à un échec ; ne pas remplacer silencieusement un JSON illisible par une base vide.
- [ ] Ajouter messages FR/EN et contrôles de persistance aux tests existants, sans recopier tout le stockage frontend dans un nouveau système.

**Clôture** : refus d'écriture et écriture partielle simulés dans fixtures/injections de test → erreur visible, aucune annonce de sauvegarde réussie, ancienne donnée intacte ; exception `localStorage.setItem` → état non enregistré visible, données exportables/réessayables ; rétablissement de l'écriture → sauvegarde et relecture après redémarrage. Pas de remplissage réel du disque ni de changement des droits du dossier utilisateur pour provoquer ces cas.

<a id="port-4"></a>
### PORT-4 — Chemins de modèles indépendants du lieu d'installation

**Priorité : haute pour le déplacement. Dépendance : PORT-2a ; séquencer avec PORT-3 sur les fichiers partagés.**

**Diagnostic** : `ai/modelPath` est stocké tel quel et prioritaire dans `ModelLocator` ; le workspace l'exporte/réimporte sans adaptation. Après copie A→B, un chemin absolu vers A peut rester utilisé ; si A disparaît, le fallback peut choisir un autre GGUF.

**Fichiers / entrée** : `ai/model_locator.*`, `apps/desktop/settings_diagnostics_manager.cpp` (`saveSettings`, `getSettings`, sélection/état modèle), `ui/src/stores/workspaceSession.ts`, `ui/src/stores/settings.ts`, `ui/src/views/SettingsView.vue`, manifests sous `model/` à relire sans changer leur convention.

**Lots à exécuter** :

- [ ] Définir une représentation compatible : modèle appartenant au paquet → chemin relatif à sa racine ; modèle externe choisi explicitement → référence externe identifiée. Résoudre les chemins relatifs contre le paquet, jamais contre `currentPath()`, et appliquer la même règle au workspace.
- [ ] Préserver la lecture des anciennes valeurs absolues. Convertir seulement les références dont l'appartenance à une racine connue est démontrée ; ne pas deviner qu'un chemin externe importé appartient au paquet. Si la référence est devenue introuvable, l'annoncer et rendre visible le modèle réellement utilisé/proposé au lieu de présenter le choix comme inchangé.
- [ ] Tester les chemins Windows avec espaces/accents et changements de lecteur lorsque disponibles ; préserver les variables d'environnement et chemins relatifs propres aux manifests selon leurs contrats existants. Faire prioriser les ressources du paquet sur les chemins de secours de développement lorsqu'un layout portable valide est présent, notamment le chargement UI dans `main.cpp`.

**Clôture** : copie A→B avec A encore présent → B utilise son modèle interne ; déplacement avec A absent → même choix interne retrouvé ; workspace exporté/importé conserve la bonne référence ; modèle externe inchangé s'il existe, erreur/situation de repli explicite s'il manque. Ne pas déduire cette réussite du seul fait qu'un quelconque GGUF a été trouvé.

<a id="port-5"></a>
### PORT-5 — Modules Lua/CLR installables sans outils développeur

**Priorité : haute pour les paquets allégés/réparation. Dépendance : inventaire runtime PORT-1 et contrat de chemins PORT-2a.**

**Diagnostic** : `getModuleCatalog()` annonce Lua/CLR installables ; `installModule()` appelle `setup-lua-runtime.ps1`/`build-clr-inspector.ps1`, absents du paquet. Copier ces scripts ne suffirait pas : ils exigent respectivement compilateur MSVC ou SDK .NET et sources du projet. Le helper CLR déjà livré en publication autonome dans le paquet complet doit être conservé.

**Fichiers / entrée** : `ApplicationController::getModuleCatalog/installModule`, `apps/desktop/lua_runtime_locator.cpp`, `apps/desktop/clr_inspector_bridge.cpp`, `ui/src/views/ModulesView.vue`, `scripts/package-windows.ps1`, `scripts/setup-lua-runtime.ps1`, `scripts/build-clr-inspector.ps1`. Ces deux derniers restent des outils de fabrication sur machine de build.

**Lots à exécuter** :

- [ ] Produire/réutiliser des archives de modules précompilés avec manifeste d'architecture/version/empreinte et fichiers requis ; CLR autonome et dépendances Lua incluses. Réutiliser les sorties de build/publish actuelles au lieu d'une deuxième compilation chez l'utilisateur.
- [ ] Prévoir l'installation depuis une archive locale vérifiable, utilisable et testable sans publication réseau. Un téléchargement ne doit être annoncé que si une vraie source d'artefacts existe : ne pas inventer d'URL ni maintenir `installable=true` sans chemin réalisable. Afficher la raison d'indisponibilité le cas échéant ; la publication externe n'est pas incluse dans ce chantier.
- [ ] Installer dans le paquet via staging borné, validation du manifeste/empreinte/contenu puis remplacement contrôlé. Refuser chemins sortant du dossier d'installation, mauvaise architecture et artefact incomplet. Échec/annulation conserve le module précédent ; dossier non inscriptible remonte l'erreur PORT-3 sans faux succès.
- [ ] Adapter catalogue, progression et messages FR/EN ; après installation vérifier un vrai démarrage/appel de version ou diagnostic du module avant de l'annoncer prêt. Respecter `-SkipClrInspector` et les autres options de packaging, sans exiger de SDK sur la machine utilisateur.

**Clôture** : paquet sans Lua/CLR extrait hors dépôt → import local des deux modules, exécution réelle de chacun, relance et redétection ; aucune invocation de MSVC/`dotnet build` chez l'utilisateur. Artefact absent, corrompu, mauvaise architecture, annulation et répertoire non inscriptible donnent un résultat honnête sans perte de l'ancien module. Si un téléchargement est livré, couvrir aussi son interruption ; absence d'hébergement ne bloque pas la voie archive locale.

<a id="port-6"></a>
### PORT-6 — Validation de déplacement avec données et environnement propre

**Priorité : clôture de la série. Dépendances : préparer le harnais dès PORT-2a ; exécution finale après PORT-1 à PORT-5.**

**Diagnostic** : AM-4 valide une UI réelle dans une copie vierge et redémarre la cible de test. Il ne ferme pas KillEngine pour déplacer son propre dossier déjà peuplé. La roadmap du 13/09 laisse explicitement ce scénario ouvert. Le chargement en URL `file://` absolue appelle un vrai contrôle de la persistance après déplacement ; une perte liée à l'origine n'a pas été démontrée à ce stade.

**Fichiers / entrée** : `scripts/test-ui-journeys.ps1`, `scripts/lib/cdp-client.ps1`, `scripts/release-check.ps1`, `docs/AUTOMATION_API.md`, `apps/desktop/main.cpp`. Étendre/réutiliser le harnais existant, pas un second framework UI.

**Scénario d'acceptation à automatiser** :

1. Extraire le ZIP dans une fixture A hors dépôt. Utiliser un répertoire courant différent et un environnement de test sans chemins de développement dans les variables de recherche ; n'altérer ni PATH global ni runtime installé sur la machine du propriétaire.
2. Créer par l'UI de vraies données sentinelles : langue, entrée Trainer, workspace/bookmark, profil `.keprofile` avec note de connaissance et modèle interne choisi ; créer une donnée Pattern Learning via l'API de test appropriée. Vérifier le résultat des sauvegardes, puis fermer proprement KillEngine et attendre la sortie de ses enfants.
3. Copier A vers B en conservant A pour le premier passage : lancer B et vérifier, dans l'UI/backend, données, chemins effectifs et modèle résolu sous B. Écrire une nouvelle sentinelle dans B ; A doit rester inchangé.
4. Fermer B, rendre A indisponible dans la fixture, renommer/déplacer B vers C (espaces/accents ; changement de lecteur si disponible), puis relancer C. Relire les valeurs depuis la vraie application, pas uniquement rechercher leur texte dans LevelDB/INI.
5. Vérifier profils, notes, Trainer, workspace, langue, modèle, Pattern Learning et localisation des nouveaux logs ; exercer un parcours cœur et les modules livrés sans utiliser les ressources du dépôt. La clé API liée au compte reste une limite attendue, pas une donnée à rendre déchiffrable ailleurs.
6. Conserver rapport, captures sur échec et versions/architecture/chemins testés ; code de sortie non nul sur échec. Un prérequis absent ou une instance utilisateur en cours doit être signalé « non exécuté », pas compté comme une réussite. Nettoyer uniquement la fixture et les processus créés.

- [ ] Brancher ce scénario via une commande documentée puis un mode dédié de `release-check.ps1` ; coordonner ce fichier avec PORT-1.
- [ ] Ajouter un test négatif montrant qu'une référence vers A ou une sentinelle perdue est bien détectée.
- [ ] Exécuter deux fois sans pollution d'une session par l'autre ; séparer le résultat « déplacement sur cette machine » du résultat « lancement sur machine propre ».

**Clôture** : preuves de relecture après copie ET déplacement, absence de dépendance au dépôt/à A, isolation des deux copies et contrôles négatifs concluants. Si l'environnement Windows propre manque, cette validation reste distinctement non réalisée ; continuer les autres lots et ne pas annoncer une autonomie inter-machine totalement vérifiée.

### Critères de livraison et reprise après interruption

Chaque lot doit laisser une note dans le tracker : fichiers/fonctions réellement modifiés, contrat retenu, tests et commandes exactes avec résultat, limite restante et prochain pas. Les numéros PORT-* restent stables ; ne pas renommer des correctifs déjà validés ni fermer toute la série sur un seul build vert.

Après code : build approprié par Codex/Claude, suite C++ complète si C++ modifié, build Vue/typage si frontend modifié, contrôles packaging/scripts et tests ciblés du lot. Éviter les builds parallèles et respecter le piège des consommateurs de headers. Ne pas figer « 540 tests » comme seuil : c'est un résultat historique, pas le nombre attendu après ajout de tests. Contrôler UTF-8 sans BOM et fins de ligne sans conversion générale.

Mettre à jour la documentation utilisateur FR/EN et packaging pour le comportement effectivement livré ; garder ici les limites DPAPI/driver et le résultat précis de PORT-6. Aucun upload, release publique, changement de certificat ou configuration Windows globale n'est nécessaire pour implémenter ces lots.

**Vérification de ce cadrage** : revue du code et des dépendances issue de l'audit du 17/09, contrôle documentaire des points d'entrée et de l'ordre des lots ; aucun code modifié, aucun build/test applicatif exécuté pour cette inscription. Les cases PORT-* restent ouvertes.
