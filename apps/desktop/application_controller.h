#pragma once

#include "ai_engine.h"
#include "candidates/candidate_store.h"
#include "debug/breakpoint_freeze.h"
#include "debug/inprocess_breakpoint.h"
#include "debug/page_guard.h"
#include "debug/speedhack.h"
#include "freeze/freeze_manager.h"
#include "inject/api_hook.h"
#include "inject/dll_injector.h"
#include "inject/function_hook.h"
#include "input/global_hotkey.h"
#include "kernel/kernel_driver_bridge.h"
#include "memory/memory_reader.h"
#include "process/process_handle.h"
#include "profiles/profile_store.h"
#include "scripting/auto_assembler.h"
#include "snapshot/snapshot_store.h"

#include <QObject>
#include <QByteArray>
#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <QPointer>

#include <memory>
#include <optional>

class QLabel;
class QProcess;
class QWidget;

namespace killengine {

/**
 * @brief Contrôleur applicatif exposé au frontend Vue via QWebChannel.
 *
 * Le frontend Vue appelle ces méthodes directement via le canal WebSocket.
 *
 * Phase 0: méthodes stub pour valider la communication.
 * Phase 1+: implémentation réelle déléguée à killcore.
 */
class ApplicationController : public QObject {
    Q_OBJECT

    // Propriétés exposées à QML/JS
    Q_PROPERTY(QString version READ version CONSTANT)
    Q_PROPERTY(bool   isAttached READ isAttached NOTIFY attachmentChanged)
    Q_PROPERTY(QString processName READ processName NOTIFY attachmentChanged)

public:
    explicit ApplicationController(QObject* parent = nullptr);
    ~ApplicationController() override;

    // -----------------------------------------------------------------------
    // Propriétés
    // -----------------------------------------------------------------------
    QString version() const;
    bool    isAttached() const;
    QString processName() const;

    // -----------------------------------------------------------------------
    // Méthodes exposées au frontend Vue (slots)
    // -----------------------------------------------------------------------

    /// Retourne la version de KillEngine.
    Q_INVOKABLE QString getVersion() const;

    /// Énumère les processus avec fenêtre visible.
    /// Retourne une liste de maps: {pid, name, path, arch, moduleCount}
    Q_INVOKABLE QVariantList getProcesses() const;

    /// Énumère les modules chargés par un processus.
    /// Retourne une liste de maps: {name, path, baseAddress, size}
    Q_INVOKABLE QVariantList getProcessModules(int pid) const;

    /// Résout "module!fonction" en adresse absolue via la table d'export PE lue
    /// dans le processus attaché (contrairement à getProcessModules, ceci marche
    /// aussi pour l'exécutable du jeu lui-même, pas seulement les DLL système).
    /// Retourne {success, address, module, function, error}.
    Q_INVOKABLE QVariantMap resolveSymbolAddress(const QString& moduleName, const QString& functionName) const;

    /// Liste les noms exportés d'un module chargé dans le processus attaché
    /// (table d'export PE lue en mémoire distante, même mécanisme que
    /// resolveSymbolAddress). Utile pour découvrir le vrai nom d'une fonction
    /// à hooker (ex: API de ressources MRM) sans deviner à l'aveugle.
    /// filterSubstring optionnel (insensible à la casse), maxNames défaut 500.
    Q_INVOKABLE QVariantMap listModuleExports(const QString& moduleName, const QString& filterSubstring, int maxNames) const;

    /// PHASE 91 — Découvre les fichiers de sauvegarde/état probables du
    /// processus attaché, en résolvant son "package family name" UWP puis en
    /// listant %LOCALAPPDATA%\Packages\<familyName>\, triés par date de
    /// modification récente d'abord. Ne fonctionne que pour les processus
    /// UWP/AppContainer (retourne success=false avec un message clair sinon).
    /// Retourne {success, familyName, files: [{path, sizeBytes, lastWriteTime}], error}.
    Q_INVOKABLE QVariantMap discoverProcessSaveFiles(int maxResults) const;

    /// PHASE 96 — Inspecte en lecture seule la ruche UWP
    /// %LOCALAPPDATA%\Packages\<familyName>\Settings\settings.dat du processus
    /// attaché. Retourne des valeurs bornées {keyPath, name, type, preview}.
    /// Ne monte rien dans HKCU/HKLM et n'expose aucune écriture registre.
    /// Retourne {success, familyName, settingsPath, values, count, error}.
    Q_INVOKABLE QVariantMap inspectProcessLocalSettings(int maxValues) const;

    /// PHASE 91 — Lit le contenu (décodé best-effort en texte imprimable,
    /// borné en taille) d'un fichier découvert via discoverProcessSaveFiles.
    /// Le chemin doit être sous %LOCALAPPDATA%\Packages\ (garde-fou : ce
    /// n'est pas une primitive de lecture de fichier arbitraire sur le disque).
    /// Retourne {success, path, text, truncated, error}.
    Q_INVOKABLE QVariantMap readProcessSaveFileText(const QString& path, int maxBytes) const;

    /// PHASE 94 — Édite en place une séquence d'octets dans un fichier de
    /// sauvegarde découvert via discoverProcessSaveFiles. findHex/replaceHex
    /// sont des octets en hexadécimal ("35 38", "35-38", casse indifférente).
    /// Garde-fous : les deux séquences doivent avoir exactement la même taille,
    /// et la séquence recherchée doit apparaître une seule fois dans le fichier.
    /// Retourne {success, path, occurrencesFound, error}.
    Q_INVOKABLE QVariantMap patchProcessSaveFileBytes(const QString& path, const QString& findHex, const QString& replaceHex);

    /// PHASE 93 — Surveille un fichier de sauvegarde (trouvé via
    /// discoverProcessSaveFiles) pour une écriture/suppression/renommage, via
    /// ReadDirectoryChangesW natif (core/process/file_watch.*). Remplace
    /// l'usage manuel de Process Monitor fait en PHASE 90. Version bloquante,
    /// utilisable directement au pipe d'automatisation (même raison que
    /// findWhatWrites vs findWhatWritesAsync : le pipe ne relit pas un signal
    /// Qt asynchrone). options keys: timeoutMs (défaut 5000, borné [250, 60000]).
    /// Même garde-fou de chemin que readProcessSaveFileText.
    /// Retourne {success, changed, changeType, cancelled, error}.
    Q_INVOKABLE QVariantMap watchSaveFileForChanges(const QString& path, const QVariantMap& options);

    /// Version non bloquante de watchSaveFileForChanges. Le résultat arrive
    /// via saveFileWatchFinished.
    Q_INVOKABLE QVariantMap startSaveFileWatchAsync(const QString& path, const QVariantMap& options);

    /// Demande l'arrêt de la surveillance de fichier en cours.
    Q_INVOKABLE QVariantMap cancelSaveFileWatch();

    /// Attache KillEngine à un processus.
    Q_INVOKABLE bool attachProcess(int pid);

    /// Détache le processus courant.
    Q_INVOKABLE void detachProcess();

    /// Retourne la carte mémoire du processus attaché.
    Q_INVOKABLE QVariantMap getMemoryMap() const;

    /// Lit un petit aperçu mémoire en hexadécimal depuis le processus attaché.
    Q_INVOKABLE QVariantMap readMemoryPreview(const QString& addressHex, int size) const;

    /// Lit une page mémoire plus large (jusqu'à 64 Ko) pour le visualiseur hexadécimal navigable.
    Q_INVOKABLE QVariantMap readMemoryBlock(const QString& addressHex, int size) const;

    /// Analyse une fenêtre mémoire en champs typés exploitables par la vue Structure.
    Q_INVOKABLE QVariantMap analyzeStructureMemory(const QString& addressHex, int size) const;

    /// Cherche une valeur affichée sous forme de texte (ASCII / UTF-16LE) dans la mémoire.
    Q_INVOKABLE QVariantMap scanUiStrings(const QString& value, const QVariantMap& options) const;

    /// Relit des candidats texte et garde ceux qui correspondent à la nouvelle valeur affichée.
    Q_INVOKABLE QVariantMap trackUiStringCandidates(const QVariantList& candidates, const QString& value) const;

    /// Cherche des valeurs numériques plausibles autour d'une string UI déjà localisée.
    Q_INVOKABLE QVariantMap analyzeUiStringSources(
        const QVariantMap& stringCandidate,
        const QString& value,
        const QVariantMap& options) const;

    /// Recherche générique d'une valeur numérique dans une fenêtre de mémoire
    /// autour d'une adresse binaire quelconque (pas une string UI — voir
    /// analyzeUiStringSources ci-dessus pour ce cas précis). Primitive dédiée
    /// (core/scanner/memory_window_search.h) plutôt que de détourner
    /// analyzeUiStringSources pour un usage générique — voir
    /// docs/STRATEGY_ROOM.md, session Solitaire du 20/08/2026.
    /// options keys: radiusBytes (défaut 65536), maxResults (défaut 200),
    /// alignment (défaut 1), excludeBytes (défaut 8, octets ignorés à partir
    /// de l'adresse ancre elle-même).
    Q_INVOKABLE QVariantMap scanMemoryWindow(
        const QString& addressHex,
        const QString& value,
        const QVariantMap& options) const;

    /// Relit des sources numériques candidates et garde celles qui suivent la nouvelle valeur affichée.
    Q_INVOKABLE QVariantMap trackUiStringSources(const QVariantList& sourceCandidates, const QString& value) const;

    /// Inspecte les strings UI suivies : cluster, slots proches et pointeurs qui les référencent.
    Q_INVOKABLE QVariantMap inspectUiStringOrigins(
        const QVariantList& stringCandidates,
        const QVariantMap& options) const;

    /// Démarre une enquête live : snapshot des zones autour des strings/sources UI suivies.
    Q_INVOKABLE QVariantMap startUiStringInvestigation(
        const QVariantList& stringCandidates,
        const QVariantList& sourceCandidates,
        const QVariantMap& options);

    /// Termine l'enquête live : relit les zones capturées et retourne les changements observés.
    Q_INVOKABLE QVariantMap finishUiStringInvestigation(const QVariantMap& options);

    /// Option douce post-crash Solitaire : capture des pages private/RW,
    /// puis compare seulement les pages réellement modifiées entre une
    /// ancienne et une nouvelle valeur affichee.
    Q_INVOKABLE QVariantMap startChangedPagesDiff(const QVariantMap& options);
    Q_INVOKABLE QVariantMap finishChangedPagesDiff(
        const QString& previousValue,
        const QString& currentValue,
        const QVariantMap& options);

    /// Lance un scan exact déterministe.
    Q_INVOKABLE QVariantMap startExactScan(const QString& value, const QString& valueType);

    /// Phase 13 : Lance un scan multi-type + variantes de représentation.
    /// Quand valueType est vide ou "Auto", cherche plusieurs représentations
    /// numériques (Int/UInt, Float, fixed-point) et applique un score de confiance.
    Q_INVOKABLE QVariantMap startExactScanMultiType(const QString& value, const QString& valueType);

    /// Lance un scan exact avec filtres Mode Expert (Phase 12).
    /// expertOptions keys: startAddress, stopAddress, alignment, writableOnly,
    ///                     executableOnly, copyOnWriteOnly, fastScan (toutes optionnelles).
    Q_INVOKABLE QVariantMap startExactScanExpert(
        const QString& value,
        const QString& valueType,
        const QVariantMap& expertOptions);

    /// Scan Expert de valeurs obfusquées simples (XOR/Add/Sub/NOT) sur les régions filtrées.
    Q_INVOKABLE QVariantMap scanEncryptedValue(const QString& value, const QString& valueType, const QVariantMap& options);

    /// Lance un scan exact dans un worker thread et retourne immédiatement un requestId.
    Q_INVOKABLE QVariantMap startExactScanAsync(
        const QString& value,
        const QString& valueType,
        const QVariantMap& expertOptions);

    /// Réduit les candidats dans un worker thread et retourne immédiatement un requestId.
    Q_INVOKABLE QVariantMap nextScanAsync(const QString& mode, const QString& value);

    /// Demande l'annulation du scan actif.
    Q_INVOKABLE QVariantMap cancelActiveScan();

    /// Réduit les candidats existants en relisant leurs adresses.
    Q_INVOKABLE QVariantMap nextScan(const QString& mode, const QString& value);

    /// Restaure la génération de candidats présente avant la dernière réduction.
    Q_INVOKABLE QVariantMap undoCandidateScan();

    /// Retourne une page de candidats issus du dernier scan.
    Q_INVOKABLE QVariantMap getCandidates(int pageIndex, int pageSize, const QString& addressFilter) const;

    /// Capture un snapshot initial sans connaître la valeur cible.
    Q_INVOKABLE QVariantMap captureUnknownSnapshot();

    /// Capture un snapshot initial avec filtres Mode Expert.
    Q_INVOKABLE QVariantMap captureUnknownSnapshotWithOptions(const QVariantMap& expertOptions);

    /// Capture un snapshot unknown dans un worker thread.
    Q_INVOKABLE QVariantMap captureUnknownSnapshotAsync();

    /// Capture un snapshot unknown dans un worker thread avec filtres Mode Expert.
    Q_INVOKABLE QVariantMap captureUnknownSnapshotAsyncWithOptions(const QVariantMap& expertOptions);

    /// Compare le snapshot unknown initial avec l'état courant.
    Q_INVOKABLE QVariantMap unknownNextScan(const QString& mode, const QString& valueType);

    /// Compare le snapshot unknown dans un worker thread.
    Q_INVOKABLE QVariantMap unknownNextScanAsync(const QString& mode, const QString& valueType);

    /// Écrit une valeur typée à une adresse.
    Q_INVOKABLE QVariantMap writeMemoryValue(const QString& addressHex, const QString& valueType, const QString& value);

    /// Écrit une valeur affichée sur plusieurs cibles, en appliquant les variantes x100/x65536/etc.
    Q_INVOKABLE QVariantMap writeMemoryValuesWithVariants(const QVariantList& targets, const QString& value);

    /// Écrit plusieurs adresses ({address, type, value}) dans la même fenêtre critique
    /// (threads de la cible suspendues pendant l'écriture) — pour les cibles qui
    /// maintiennent des copies redondantes d'une même valeur et détectent/annulent
    /// une écriture isolée (roadmap I, session Solitaire du 19/08/2026, voir
    /// docs/STRATEGY_ROOM.md). options: { suspendThreads: bool (def. true) }.
    Q_INVOKABLE QVariantMap writeMemoryValuesAtomic(const QVariantList& targets, const QVariantMap& options);

    /// Restaure la dernière valeur écrasée par writeMemoryValue.
    Q_INVOKABLE QVariantMap rollbackLastWrite();

    /// Restaure toutes les écritures du dernier batch (auto-write).
    Q_INVOKABLE QVariantMap rollbackLastWriteBatch();

    /// Active/désactive un freeze simple sur une adresse.
    Q_INVOKABLE QVariantMap setFreezeValue(const QString& addressHex, const QString& valueType, const QString& value, bool enabled);

    /// Active un freeze par hardware breakpoint sur une adresse (mode Expert).
    Q_INVOKABLE QVariantMap freezeWithBreakpoint(const QString& addressHex, const QString& valueType, const QString& value, const QVariantMap& options);

    /// Fait passer une adresse déjà en freeze polling vers le mode hardware
    /// breakpoint, sans que l'appelant ait besoin de reconnaître type/valeur :
    /// réutilise l'entrée FreezeEntry existante. Pensé pour le bouton "Passer
    /// en Freeze BP" proposé par l'Assistant après une détection d'instabilité
    /// (freezeInstabilityDetected) — l'utilisateur n'a que l'adresse en main.
    Q_INVOKABLE QVariantMap escalatePollingFreezeToBreakpoint(const QString& addressHex);

    /// Arrête le freeze par hardware breakpoint actif.
    Q_INVOKABLE QVariantMap stopBreakpointFreeze();

    /// Statistiques live du freeze par hardware breakpoint actif (hits,
    /// réécritures, erreurs). Jusqu'ici collectées par BreakpointFreezeManager
    /// mais jamais exposées à l'UI en dehors de l'arrêt — l'utiliser pour
    /// afficher une tenue en direct plutôt que d'attendre stopBreakpointFreeze.
    Q_INVOKABLE QVariantMap getBreakpointFreezeStats() const;

    // -----------------------------------------------------------------------
    // Phase 17 — Hardware Breakpoints / Find What Writes
    // -----------------------------------------------------------------------

    /// Trouve l'instruction qui écrit à une adresse (hardware breakpoint).
    /// C'est LA fonction qui permet de remonter à la vraie source gameplay
    /// quand on a trouvé une valeur "displayed" qui est réécrite par le jeu.
    /// options keys: size (1/2/4/8), timeoutMs (défaut 5000), maxHits (défaut 10)
    Q_INVOKABLE QVariantMap findWhatWrites(const QString& addressHex, const QVariantMap& options);

    /// Version non bloquante de findWhatWrites. Le résultat arrive via findWhatWritesFinished.
    Q_INVOKABLE QVariantMap findWhatWritesAsync(const QString& addressHex, const QVariantMap& options);

    /// Demande l'arrêt de la capture Find What Writes en cours.
    Q_INVOKABLE QVariantMap cancelFindWhatWrites();

    /// Find What Accesses (breakpoint lecture/ecriture) : capture les instructions qui LISSENT l'adresse.
    /// Version bloquante (miroir de findWhatWrites) — utile notamment pour le
    /// connecteur d'automatisation (le pipe ne peut pas relire le resultat
    /// d'un signal Qt asynchrone, voir docs/STRATEGY_ROOM.md session Solitaire).
    Q_INVOKABLE QVariantMap findWhatAccesses(const QString& addressHex, const QVariantMap& options);

    /// Version non bloquante. Le resultat arrive via findWhatAccessesFinished.
    Q_INVOKABLE QVariantMap findWhatAccessesAsync(const QString& addressHex, const QVariantMap& options);

    /// Break on execute : capture quand une instruction connue s'execute et
    /// retourne les registres runtime (R8/RBP/XMM0...). Utile quand un hit
    /// pointe vers une copie UI mais que la destination varie a chaque frame.
    Q_INVOKABLE QVariantMap findWhatExecutes(const QString& instructionAddressHex, const QVariantMap& options);

    /// Variante in-process/non DebugActiveProcess de findWhatExecutes. Utilise
    /// le composant injecte existant et expose les derniers registres captures
    /// par le VEH (couverture bornee aux threads armees par ce composant).
    Q_INVOKABLE QVariantMap startInProcessExecuteWatchAsync(const QString& instructionAddressHex, const QVariantMap& options);

    /// Version bloquante de startInProcessExecuteWatchAsync (miroir de
    /// findWhatWrites vs findWhatWritesAsync) : le pipe d'automatisation ne
    /// peut pas relire le résultat d'un signal Qt asynchrone
    /// (inProcessBreakpointWatchFinished), donc c'est celle-ci qu'un agent
    /// pilotant KillEngine via le pipe doit utiliser pour obtenir les
    /// registres capturés (RAX/RCX/RDX/RBP/RSP/R8/R9/XMM0) directement.
    Q_INVOKABLE QVariantMap startInProcessExecuteWatch(const QString& instructionAddressHex, const QVariantMap& options);

    /// Lecture opportuniste de texte de fenetres/controles du PID attache
    /// (UI Automation/OCR light). Ne lit pas la memoire du jeu ; sert a
    /// synchroniser la valeur affichee quand Windows expose du texte.
    Q_INVOKABLE QVariantMap readAttachedWindowText(const QVariantMap& options) const;

    /// Descend dans l'arbre UI Automation (IUIAutomation) d'une fenêtre liée
    /// au processus attaché et retourne le Name/Value de chaque élément
    /// (Descendants). Contrairement à readAttachedWindowText (titres de
    /// fenêtres Win32 seulement), ceci lit le texte des contrôles individuels
    /// (ex: un TextBlock XAML "59" ou "Bulles") directement via l'API
    /// d'accessibilité Windows — sans toucher à la mémoire du processus, donc
    /// aucun risque de crash lié à un debugger. Sert d'oracle de vérité fiable
    /// pour valider un candidat mémoire (comparer la valeur UIA à la valeur
    /// lue à une adresse), ou pour retrouver la valeur affichée quand le scan
    /// mémoire échoue (voir docs/STRATEGY_ROOM.md, session Solitaire Bulles,
    /// 24/08/2026).
    /// options: hwndHex (optionnel, sinon réutilise l'heuristique de
    /// readAttachedWindowText avec titleContains), titleContains (défaut
    /// "Solitaire"), maxElements (défaut 500), filterText (sous-chaîne
    /// optionnelle sur Name/Value pour réduire le bruit).
    Q_INVOKABLE QVariantMap readUiAutomationTree(const QVariantMap& options) const;

    /// Scan groupe : cherche N valeurs avec offsets fixes connus (ex: HP/Mana/Stamina voisins).
    /// Entrees : liste {offset, type, value} + options standards Mode Expert.
    Q_INVOKABLE QVariantMap scanGroupScan(const QVariantList& entries, const QVariantMap& options);

    /// Alternative à findWhatWritesAsync qui n'attache pas de débogueur externe : capture via
    /// PAGE_GUARD + composant injecté au lieu d'un hardware breakpoint Win32 (DebugActiveProcess).
    /// Utile quand un autre débogueur est déjà attaché à la cible (le canal de debug Win32
    /// n'accepte qu'un seul propriétaire) ou pour de l'instrumentation qui doit rester active
    /// indépendamment de l'état du canal de debug.
    /// Version non bloquante. Le résultat arrive via pageGuardWatchFinished.
    /// options keys: size (1-4096, défaut 4), captureWrites (défaut true), captureReads, timeoutMs, maxHits.
    Q_INVOKABLE QVariantMap startPageGuardWatchAsync(const QString& addressHex, const QVariantMap& options);

    /// Demande l'arrêt de la capture Page Guard en cours.
    Q_INVOKABLE QVariantMap cancelPageGuardWatch();

    /// Variante plus précise de startPageGuardWatchAsync : hardware breakpoint (DR0, adresse
    /// exacte plutôt que la page de 4 Ko) posé depuis un composant injecté dans la cible — voir
    /// core/debug/inprocess_breakpoint.h. Version non bloquante, résultat via
    /// inProcessBreakpointWatchFinished. options keys: size (1/2/4/8, défaut 4),
    /// captureWrites (défaut true), timeoutMs, maxHits.
    Q_INVOKABLE QVariantMap startInProcessBreakpointWatchAsync(const QString& addressHex, const QVariantMap& options);

    /// Demande l'arrêt de la capture breakpoint in-process en cours.
    Q_INVOKABLE QVariantMap cancelInProcessBreakpointWatch();

    /// Freeze via breakpoint in-process : le composant injecté réécrit lui-même la valeur figée
    /// juste après chaque écriture interceptée, sans jamais attacher de débogueur externe (à la
    /// différence de freezeWithBreakpoint qui, lui, dépend du canal de debug Win32). Reste actif
    /// jusqu'à stopInProcessBreakpointFreeze().
    Q_INVOKABLE QVariantMap startInProcessBreakpointFreeze(const QString& addressHex, const QString& valueType, const QString& value, const QVariantMap& options);

    /// Arrête le freeze breakpoint in-process actif.
    Q_INVOKABLE QVariantMap stopInProcessBreakpointFreeze();

    /// Stats en direct du freeze breakpoint in-process actif (hitCount, threads armées) —
    /// lecture directe de la mémoire partagée, pas d'attente de l'arrêt.
    Q_INVOKABLE QVariantMap getInProcessBreakpointFreezeStats() const;

    /// Roadmap section J — Speedhack : accélère/ralentit le temps perçu par le
    /// processus attaché (hook des fonctions de temps depuis un composant injecté,
    /// voir core/debug/speedhack.h). factor=1.0 vitesse normale, factor=0.0 pause.
    /// Réutilise un composant déjà installé sur cette cible s'il existe (pas besoin
    /// de réinjecter pour réactiver, voir SpeedhackSession::start).
    Q_INVOKABLE QVariantMap startSpeedhack(double factor);

    /// Change le facteur en direct sans réinjecter — pour un slider côté UI.
    Q_INVOKABLE QVariantMap setSpeedhackFactor(double factor);

    /// Remet le facteur à 1.0 (vitesse normale) ; ne dé-injecte jamais (voir
    /// core/debug/speedhack.h pour pourquoi).
    Q_INVOKABLE QVariantMap stopSpeedhack();

    /// Roadmap section B - Interception de fonctions : pose un hook MinHook
    /// in-process (composant injecte KillEngineApiHookHandler.dll) sur
    /// module!fonction pour compter les appels et/ou forcer la valeur de
    /// retour. mode: 0 = compter seulement, 1 = forcer le retour.
    Q_INVOKABLE QVariantMap startApiHook(const QString& moduleName, const QString& functionName,
                                        int mode, qlonglong forcedReturnValue);

    /// Retire le hook et referme la session (le composant reste charge).
    Q_INVOKABLE QVariantMap stopApiHook();

    /// Statut courant (actif, compteurs d appels, erreurs).
    Q_INVOKABLE QVariantMap getApiHookStatus() const;

    /// Statut courant du speedhack (actif, facteur, fonctions hookées).
    Q_INVOKABLE QVariantMap getSpeedhackStatus() const;

    /// Ecriture hexadecimale brute : "48 8B 00" -> bytes exacts a l'adresse. Sauvegarde previous pour rollback.
    Q_INVOKABLE QVariantMap writeMemoryHex(const QString& addressHex, const QString& hexString);

    /// Dump d'une region memoire vers fichier binaire (.bin) sous QStandardPaths::DocumentsLocation/KillEngine/dumps.
    Q_INVOKABLE QVariantMap dumpMemoryRegion(const QString& addressHex, int size, const QString& fileName);

    /// Cherche une signature AOB dans les régions mémoire du processus.
    /// Pattern: "48 8B ?? ?? 89", options: executableOnly, imageOnly, startAddress, stopAddress, maxResults.
    Q_INVOKABLE QVariantMap scanAobPattern(const QString& pattern, const QVariantMap& options);

    /// Génère une signature AOB exacte à partir des octets autour d'une adresse d'instruction.
    /// options keys: beforeBytes, length.
    Q_INVOKABLE QVariantMap generateAobSignature(const QString& addressHex, const QVariantMap& options);

    /// Applique un patch de bytes exacts à une adresse code, en gardant les bytes originaux pour restauration.
    /// bytesText: "90 90 CC", options keys: verify.
    Q_INVOKABLE QVariantMap applyCodePatch(const QString& addressHex, const QString& bytesText, const QVariantMap& options);

    /// Analyse les bytes à une adresse et propose des patchs de même longueur.
    Q_INVOKABLE QVariantMap suggestCodePatches(const QString& addressHex, const QVariantMap& options);

    /// Désassemble les instructions menant à addressHex (ex: le RIP d'un hit
    /// findWhatWrites), en cherchant en arrière depuis quelques dizaines
    /// d'octets avant. Lecture mémoire seule, aucune écriture, aucun attach
    /// debugger. options keys: windowBytes (défaut 64, borné 16-128).
    Q_INVOKABLE QVariantMap disassembleBackward(const QString& addressHex, const QVariantMap& options) const;

    /// Teste automatiquement lequel des champs candidats de disassembleBackward()
    /// est la vraie source d'un compteur animé : écrit une valeur test sur chaque
    /// champ résolu (voir killcore::resolveCandidateFieldAddresses), attend
    /// quelques secondes, relit, classe holds/reverts, puis restaure — pour
    /// remplacer la lecture manuelle d'assembleur par une preuve empirique.
    /// writeInstructionAddressHex = RIP de l'instruction d'écriture capturée
    /// (même entrée que disassembleBackward). knownWriteTargetAddressHex =
    /// adresse mémoire réellement écrite (hit.address d'un hit findWhatWrites).
    /// Non bloquant. Le résultat arrive via candidateFieldTestFinished.
    Q_INVOKABLE QVariantMap testCandidateFieldsAsync(const QString& writeInstructionAddressHex, const QString& knownWriteTargetAddressHex, const QVariantMap& options);

    /// Demande l'arrêt du test de champs candidats en cours.
    Q_INVOKABLE QVariantMap cancelCandidateFieldTest();

    /// Restaure les bytes originaux d'un patch actif.
    Q_INVOKABLE QVariantMap restoreCodePatch(const QString& addressHex);

    // -----------------------------------------------------------------------
    // Phase 20 — Injection / hooking / auto-assembler (outils Expert manuels
    // gardés par confirmation explicite côté frontend, cf. confirmRiskAction
    // risk='injection'). Pas encore de feature Trainer persistante (action
    // 'hook') : ces méthodes sont pour l'instant du one-shot Expert, comme les
    // patchs AOB avant d'avoir leur propre étage Trainer.

    /// Injecte une DLL dans le processus attaché (CreateRemoteThread + LoadLibraryW).
    Q_INVOKABLE QVariantMap injectDllIntoProcess(const QString& dllPath);

    /// Installe un inline hook (detour) sur une fonction du processus attaché.
    /// Garde les bytes originaux pour restauration via removeFunctionHook.
    Q_INVOKABLE QVariantMap installFunctionHook(const QString& targetAddressHex, const QString& hookAddressHex);

    /// Retire un hook actif et restaure les bytes originaux.
    Q_INVOKABLE QVariantMap removeFunctionHook(const QString& targetAddressHex);

    /// Parse et compile un script auto-assembler sans l'exécuter (aperçu : instructions reconnues, erreurs de syntaxe/ligne).
    Q_INVOKABLE QVariantMap parseAutoAssemblerScript(const QString& scriptText) const;

    /// Exécute un script auto-assembler dans le processus attaché ; garde le résultat pour restoreAutoAssemblerScript.
    Q_INVOKABLE QVariantMap executeAutoAssemblerScript(const QString& scriptText);

    /// Restaure les bytes originaux du dernier script auto-assembler exécuté.
    Q_INVOKABLE QVariantMap restoreAutoAssemblerScript();

    /// Génère et exécute automatiquement un script auto-assembler CE-style
    /// (trampoline alloué + redirection du site d'écriture capturé par
    /// "Écrit par") qui force `value` à l'adresse mémoire visée par
    /// l'instruction, quelle que soit sa source (immédiat OU registre) —
    /// contrairement à "Forcer une valeur" (patch d'octets), qui ne marche
    /// que pour un immédiat littéral. Restaurable via restoreAutoAssemblerScript.
    Q_INVOKABLE QVariantMap forceWriteInstructionValue(
        const QString& ripHex,
        int instructionLength,
        const QString& memBaseRegister,
        qlonglong memDisplacement,
        const QString& valueType,
        const QString& value);

    /// Configure l'intervalle du freeze polling (10-2000 ms, 100 ms par défaut).
    Q_INVOKABLE QVariantMap setFreezeInterval(int intervalMs);

    /// Enregistre une hotkey globale et renvoie son ID.
    Q_INVOKABLE QVariantMap registerGlobalHotkey(const QString& combo, const QVariantMap& action);

    /// Supprime une hotkey globale.
    Q_INVOKABLE QVariantMap unregisterGlobalHotkey(int id);

    /// Liste les hotkeys globales enregistrées.
    Q_INVOKABLE QVariantMap getGlobalHotkeys() const;

    /// Supprime toutes les hotkeys globales.
    Q_INVOKABLE QVariantMap clearGlobalHotkeys();

    /// Affiche/masque l'overlay Trainer externe always-on-top.
    Q_INVOKABLE QVariantMap setTrainerOverlayVisible(bool visible, const QVariantMap& options);

    /// Met à jour le contenu de l'overlay Trainer.
    Q_INVOKABLE QVariantMap updateTrainerOverlay(const QVariantMap& state);

    /// Lance une recherche intelligente (Smart Search).
    /// Phase 0: stub qui logge la requête.
    Q_INVOKABLE QVariantMap startSmartSearch(const QString& query);

    /// Lance une auto-résolution prudente : plan IA + premières actions sûres seulement.
    Q_INVOKABLE QVariantMap startAutoResolve(const QString& query, const QVariantMap& options);

    /// Résume le contexte d'enquête et la télémétrie récente pour guider l'IA proactive.
    Q_INVOKABLE QVariantMap getAutoResolveReport(int maxEvents) const;

    /// Vide la mémoire locale d'auto-résolution (QSettings) pour le processus courant ou tous les processus.
    Q_INVOKABLE QVariantMap clearAutoResolveMemory(bool allProcesses);

    /// Ajoute un événement d'audit IA dans la télémétrie locale.
    Q_INVOKABLE QVariantMap logAiAudit(const QString& event, const QVariantMap& payload);

    /// Motifs mémorisés (module + offset relatif + type + AOB) pour l'exécutable attaché,
    /// résolus en adresses live si un processus est attaché — évite de repartir d'un scan
    /// à froid pour une cible déjà identifiée lors d'une session précédente sur ce jeu.
    Q_INVOKABLE QVariantMap getRememberedPatterns() const;

    /// Séquence ordonnée des dernières écritures confirmées (module + offset relatif),
    /// persistée par exécutable — contrairement à rememberedPatterns (dédupliqué par
    /// cible), garde l'ordre et les doublons pour permettre un replay fidèle.
    Q_INVOKABLE QVariantMap getWriteHistorySequence() const;

    /// Rejoue dans l'ordre la séquence persistée d'écritures pour l'exécutable attaché
    /// (module non chargé ou résolution échouée = entrée ignorée, reste dans le rapport).
    Q_INVOKABLE QVariantMap replayWriteHistorySequence();

    /// Vide la séquence d'écritures persistée pour l'exécutable attaché.
    Q_INVOKABLE QVariantMap clearWriteHistorySequence();

    /// Ping — permet au frontend de vérifier que le backend est connecté.
    Q_INVOKABLE QString ping(const QString& message);

    /// Retourne les paramètres persistants de l'application.
    Q_INVOKABLE QVariantMap getSettings() const;

    /// Vrai si la modale de bienvenue première ouverture a déjà été vue/fermée (QSettings, survit à un profil Windows différent).
    Q_INVOKABLE bool hasSeenOnboarding() const;

    /// Marque la modale de bienvenue comme vue (case "Ne plus afficher").
    Q_INVOKABLE void setOnboardingSeen(bool seen);

    /// Ouvre USER_GUIDE.md dans l'application par défaut du système (package: à côté de l'exe ; dev: docs/USER_GUIDE.md).
    Q_INVOKABLE bool openUserGuide() const;

    /// Demande une exclusion Windows Defender (protection temps réel) pour le
    /// dossier d'installation et KillEngine.exe — déclenche une invite UAC
    /// visible (élévation explicite), n'agit que si l'utilisateur accepte.
    /// Nécessaire car un cycle debug externe (findWhatWrites) suivi d'une
    /// injection in-process peut être bloqué par certains EDR/antivirus qui
    /// traitent cette séquence comme une heuristique d'injection de code
    /// malveillante — voir docs/STRATEGY_ROOM.md, 20/08/2026. Ne fait RIEN
    /// silencieusement : cette méthode existe précisément pour que ce soit
    /// toujours un choix explicite de l'utilisateur, jamais automatique.
    Q_INVOKABLE QVariantMap requestWindowsDefenderExclusion();

    /// Coupe l'accès réseau (entrant + sortant) du processus attaché via une
    /// règle pare-feu Windows dédiée à son exécutable — déclenche une invite
    /// UAC visible (élévation explicite pour New-NetFirewallRule, jamais
    /// silencieux). Cas d'usage : isoler si une valeur instable en mémoire
    /// vient d'une synchro serveur en arrière-plan plutôt que d'une
    /// réallocation purement locale (testé en pratique sur Solitaire, voir
    /// docs/STRATEGY_ROOM.md, 24/08/2026). La règle persiste après un
    /// detachProcess() — appeler unblockProcessNetwork() pour la retirer.
    Q_INVOKABLE QVariantMap blockProcessNetwork();

    /// Retire la règle posée par blockProcessNetwork() pour le processus
    /// attaché (ou pour le dernier exécutable bloqué si entretemps détaché).
    /// Déclenche aussi une invite UAC (Remove-NetFirewallRule).
    Q_INVOKABLE QVariantMap unblockProcessNetwork();

    /// Etat courant de blocage réseau pour le processus attaché (ou le
    /// dernier exécutable bloqué). Lecture seule (Get-NetFirewallRule), pas
    /// d'élévation nécessaire.
    Q_INVOKABLE QVariantMap getProcessNetworkBlockStatus() const;

    /// Inspecteur CLR/ClrMD externe : lance le helper .NET si
    /// necessaire puis dialogue avec lui via JSON-RPC sur named pipe.
    Q_INVOKABLE QVariantMap getClrInspectorStatus() const;
    Q_INVOKABLE QVariantMap attachClrInspector();
    Q_INVOKABLE QVariantMap detachClrInspector();
    Q_INVOKABLE QVariantMap shutdownClrInspector();
    Q_INVOKABLE QVariantMap flushClrInspectorCache();
    Q_INVOKABLE QVariantMap findClrObjectsByType(const QString& typeSubstring);
    Q_INVOKABLE QVariantMap findClrObjectsByFieldValue(const QString& typeSubstring, const QString& fieldName, const QString& expectedValue, int maxResults);
    Q_INVOKABLE QVariantMap readClrObject(const QString& addressHex);
    Q_INVOKABLE QVariantMap writeClrPrimitiveField(const QString& objectAddressHex, const QString& fieldName, const QString& value);
    Q_INVOKABLE QVariantMap writeClrPrimitivePath(const QString& objectAddressHex, const QString& path, const QString& value);
    Q_INVOKABLE QVariantMap writeClrPrimitivePathBatch(const QString& objectAddressHex, const QVariantList& operations);
    Q_INVOKABLE QVariantMap enumerateClrRoots(const QString& typeSubstring);

    /// PHASE 59 : variantes "locator" de writeClrPrimitivePath/Batch --
    /// relocalisent l'objet root via findClrObjectsByFieldValue (meme
    /// mecanisme que resolveProfileTarget/activateProfileTarget pour
    /// LocatorKind::ClrField) juste avant d'ecrire, au lieu d'exiger que
    /// l'appelant fournisse une adresse potentiellement perimee (deplacee par
    /// un GC compactant depuis la derniere lecture). Ne dupliquent pas la
    /// logique d'ecriture : resolvent l'adresse fraiche puis delegue a
    /// writeClrPrimitivePath/writeClrPrimitivePathBatch. Erreur claire si 0
    /// ou plus d'1 objet ne correspond au locator (meme discipline que
    /// saveClrFieldProfileTarget) -- pas d'ecriture sur un locator ambigu.
    Q_INVOKABLE QVariantMap writeClrPrimitivePathByLocator(const QString& typeSubstring, const QString& identityField, const QString& identityValue, const QString& path, const QString& value);
    Q_INVOKABLE QVariantMap writeClrPrimitivePathBatchByLocator(const QString& typeSubstring, const QString& identityField, const QString& identityValue, const QVariantList& operations);

    /// PHASE 59 : variante "atomique" de writeClrPrimitivePathBatch --
    /// suspend TOUTES les threads du processus attache
    /// (killcore::ProcessThreadsSuspendGuard, meme primitive RAII que
    /// writeMemoryValuesAtomic) pendant tout l'appel RPC vers le helper
    /// ClrMD, puis reprend les threads avant de retourner le resultat. Le
    /// helper .NET lui-meme ignore que le process est suspendu -- la
    /// garantie est apportee entierement cote natif, autour de l'appel RPC
    /// complet. Risque documente honnetement (pas une atomicite parfaite) :
    /// suspendre toutes les threads pendant une operation ClrMD peut
    /// interagir avec le GC/JIT si l'un de ces sous-systemes attendait un
    /// signal d'une thread desormais suspendue -- best-effort, comme le
    /// reste du module pour les fenetres GC. Voir
    /// docs/KILLENGINE_CLR_INSPECTOR_SPEC.md.
    Q_INVOKABLE QVariantMap writeClrPrimitivePathBatchAtomic(const QString& objectAddressHex, const QVariantList& operations);

    /// Appelle REELLEMENT un setter de propriete C# d'instance dans le
    /// processus attache (pas une ecriture memoire brute du champ backing) :
    /// resout l'adresse native deja JITtee du setter via le helper ClrMD
    /// (RPC resolveInstanceMethodAddress), construit un shellcode x64 fixe
    /// (this en RCX, valeur optionnelle en RDX -- ou en XMM1 si le parametre
    /// est Single/Double, voir buildCallInstanceMethodShellcode, PHASE 59),
    /// l'injecte via killcore::injectShellcode puis relit l'objet pour
    /// verifier best-effort. Categoriquement different des autres ecritures
    /// CLR de ce fichier : injecte et EXECUTE du code dans la cible (gate de
    /// risque 'injection' obligatoire cote frontend, comme
    /// injectDllIntoProcess/installFunctionHook). Perimetre : setters
    /// d'INSTANCE uniquement, 0 ou 1 parametre PRIMITIF (bool/int8..
    /// int64/uint8..uint64/Single/Double) -- pas string/objet/struct.
    /// valueType peut etre laisse vide (le type reel resolu par ClrMD pilote
    /// l'encodage), ou fourni pour une verification de coherence
    /// supplementaire avant l'injection. Voir
    /// docs/KILLENGINE_CLR_INSPECTOR_SPEC.md pour le detail complet.
    Q_INVOKABLE QVariantMap callClrInstanceMethod(const QString& objectAddressHex, const QString& methodName, const QString& valueText, const QString& valueType);

    /// Reconstruit un chemin root -> ... -> objet cible a travers plusieurs
    /// sauts de references (chantier "GCRoot chain complet", docs/
    /// KILLENGINE_CLR_INSPECTOR_SPEC.md) -- delegue integralement au helper
    /// ClrMD (RPC findGcRootPath), aucune logique cote natif au-dela du
    /// relai. Point le plus exploratoire du lot : un chemin trouve n'est pas
    /// garanti le plus court, et un tas volumineux peut rendre l'appel lent
    /// -- timeout deliberement plus large que les autres methodes RPC
    /// (voir l'appel a callClrInspectorRpc dans l'implementation).
    Q_INVOKABLE QVariantMap findClrGcRootPath(const QString& targetObjectAddressHex, int maxDepth, int maxRootsScanned);

    /// Genere un rapport borne de l'objet donne et de son graphe atteignable
    /// (BFS sur les references, chaque noeud decrit comme readObject, plus le
    /// chemin GCRoot optionnel vers l'objet racine) via ClrMD (RPC
    /// generateObjectReport), aucune logique cote natif au-dela du relai.
    /// maxDepth/maxNodes a 0 = valeurs par defaut cote helper (3/50).
    Q_INVOKABLE QVariantMap generateClrObjectReport(const QString& objectAddressHex, int maxDepth, int maxNodes, bool includeGcRootChain);

    /// Desassemble le code natif deja JITte d'une methode CLR (setter ou
    /// autre) resolue via la meme voie que callClrInstanceMethod
    /// (resolveInstanceMethodAddress cote helper ClrMD) : lit un buffer de
    /// bytes depuis l'adresse native resolue via killcore::MemoryReader, puis
    /// desassemble en avant via killcore::disassembleForwardWindow
    /// (core/patch/instruction_patch_suggester.h) -- reutilise le decodeur
    /// x64 existant, ne le reimplemente pas. Lecture seule, aucune injection.
    Q_INVOKABLE QVariantMap disassembleClrMethod(const QString& objectAddressHex, const QString& methodName, int instructionCount);

    /// Probe le driver noyau optionnel KillEngineKernel.sys (health check uniquement).
    Q_INVOKABLE QVariantMap probeKernelDriver() const;

    /// Démarre le service Windows visible `KillEngineKernel` s'il est déjà
    /// installé mais arrêté, puis relance un probe. Ne crée ni n'installe le
    /// service : action de maintenance explicite, admin/test-signing requis.
    Q_INVOKABLE QVariantMap startKernelDriver() const;

    /// Lit `size` octets (1-4096) sur le processus attaché via le driver noyau
    /// (KeStackAttachProcess côté driver.cpp, pas ReadProcessMemory usermode).
    /// Nécessite un driver connecté avec capabilities.processMemoryAccess=true
    /// (voir probeKernelDriver) — échoue proprement sinon, jamais de crash.
    Q_INVOKABLE QVariantMap readMemoryKernel(const QString& addressHex, int size) const;

    /// Écrit des octets (chaîne hexadécimale, ex. "90 90 90") à l'adresse
    /// donnée sur le processus attaché via le driver noyau. Contourne les
    /// protections mémoire usermode normales (VirtualProtect/PAGE_GUARD) —
    /// action à risque équivalente à une injection côté UI (confirmRiskAction).
    Q_INVOKABLE QVariantMap writeMemoryKernel(const QString& addressHex, const QString& hexBytes);

    /// Comme writeMemoryKernel, mais prend une valeur typée (ex: valueType="Int32",
    /// value="9999") au lieu d'octets hexadécimaux bruts — réutilise le même
    /// parsing que writeMemoryValue (killcore::parseValueType/parseScanValue),
    /// pour l'écriture kernel déclenchée par l'Assistant (checkpoint
    /// kernel_write) ou le bouton d'escalade en mode Expert.
    Q_INVOKABLE QVariantMap writeMemoryValueKernel(const QString& addressHex, const QString& valueType, const QString& value);

    /// Retourne un diagnostic lisible du runtime IA local (modèle GGUF + llama-cli).
    Q_INVOKABLE QVariantMap getAiModelStatus() const;

    /// Ouvre un sélecteur de fichier natif pour choisir un modèle GGUF (remplace la saisie manuelle du chemin).
    Q_INVOKABLE QVariantMap browseForModelFile();

    /// Sauvegarde les paramètres persistants de l'application.
    Q_INVOKABLE QVariantMap saveSettings(const QVariantMap& settings);

    /// Retourne le chemin du fichier de log.
    Q_INVOKABLE QString getLogFilePath() const;

    /// Retourne le chemin du fichier debug Smart Search.
    Q_INVOKABLE QString getSmartSearchDebugFilePath() const;

    /// Retourne le chemin du fichier telemetry des scans.
    Q_INVOKABLE QString getScanTelemetryFilePath() const;

    /// Retourne les derniers événements du debug Smart Search.
    Q_INVOKABLE QVariantMap getSmartSearchDebugEvents(int maxEvents) const;

    /// Vide le fichier debug Smart Search.
    Q_INVOKABLE QVariantMap clearSmartSearchDebugEvents();

    /// Retourne les dernières lignes du fichier de log principal.
    Q_INVOKABLE QVariantMap getLogTail(int maxLines) const;

    /// Exporte logs et diagnostics dans une archive zip locale.
    Q_INVOKABLE QVariantMap exportDiagnostics();

    /// Liste les adresses mémoire actives dans l'Assistant.
    Q_INVOKABLE QVariantMap getActiveChatMemoryTargets() const;

    /// Oublie les adresses mémoire actives dans l'Assistant.
    Q_INVOKABLE QVariantMap clearActiveChatMemoryTargets();

    /// Vide le contexte de scan (candidats, undo, snapshot unknown) pour repartir proprement.
    Q_INVOKABLE QVariantMap clearScanContext();

    /// A appeler quand le frontend répond à une relance de l'échelle de secours
    /// (buildFailureEscalationRecovery) via un bouton plutôt qu'en tapant dans le
    /// chat (ex. bouton "Tracer le texte affiché") : ces boutons court-circuitent
    /// startSmartSearch, donc rien d'autre ne consommerait sinon m_pendingRecoveryAction.
    Q_INVOKABLE void acknowledgePendingSmartSearchRecovery();

    /// Retourne l'état du stockage temporaire des scans.
    Q_INVOKABLE QVariantMap getTemporaryStorageStatus() const;

    /// Ferme les stores temporaires actifs et supprime les fichiers temporaires KillEngine restants.
    Q_INVOKABLE QVariantMap clearTemporaryStorage();

    /// Retourne le contexte structuré courant de l'Assistant.
    Q_INVOKABLE QVariantMap getSmartSearchContext() const;

    // -----------------------------------------------------------------------
    // Phase 11 — Profils
    // -----------------------------------------------------------------------

    /// Sauvegarde le candidat sélectionné comme cible dans un profil.
    /// Crée un locator module_offset pour retrouver l'adresse après redémarrage.
    Q_INVOKABLE QVariantMap saveProfileTarget(
        const QString& profileName,
        const QString& targetName,
        const QString& addressHex,
        const QString& valueType,
        const QString& description);

    /// Sauvegarde une cible CLR par locator symbolique :
    /// type + champ d'identité + valeur -> objet courant -> champ cible.
    Q_INVOKABLE QVariantMap saveClrFieldProfileTarget(
        const QString& profileName,
        const QString& targetName,
        const QString& typeSubstring,
        const QString& identityField,
        const QString& identityValue,
        const QString& targetField,
        const QString& valueType,
        const QString& description);

    /// Liste tous les profils disponibles.
    Q_INVOKABLE QVariantList listProfiles();

    /// Charge un profil et retourne ses cibles.
    Q_INVOKABLE QVariantMap loadProfile(const QString& profileName);

    /// Supprime un profil.
    Q_INVOKABLE bool deleteProfile(const QString& profileName);

    /// Résout une cible de profil en adresse absolue pour le processus courant.
    Q_INVOKABLE QVariantMap resolveProfileTarget(const QString& profileName, const QString& targetName);

    /// Roadmap section L — Pointer maps : résout TOUTES les cibles d'un profil
    /// d'un coup sur le processus attaché (utile après un redémarrage du jeu),
    /// pour un diagnostic groupé au lieu de revalider chaque cible une par une.
    /// Retourne {success, profileName, results: [{targetName, locatorKind,
    /// status, address, previousAddress}], validCount, invalidCount}.
    Q_INVOKABLE QVariantMap comparePointerMapAcrossRestart(const QString& profileName);

    /// Active une cible de profil pour l'utiliser directement depuis l'Assistant.
    Q_INVOKABLE QVariantMap activateProfileTarget(const QString& profileName, const QString& targetName);

    /// Sauvegarde un patch code trainer dans un profil.
    Q_INVOKABLE QVariantMap saveProfileCodePatch(
        const QString& profileName,
        const QString& patchName,
        const QString& addressHex,
        const QString& aobPattern,
        const QString& patchBytes,
        const QVariantMap& metadata);

    /// Retrouve un patch par AOB stable puis applique ses bytes.
    Q_INVOKABLE QVariantMap applyProfileCodePatch(const QString& profileName, const QString& patchName);

    /// Restaure un patch de profil déjà appliqué dans la session courante.
    Q_INVOKABLE QVariantMap restoreProfileCodePatch(const QString& profileName, const QString& patchName);

    /// Applique tous les patchs code d'un profil.
    Q_INVOKABLE QVariantMap applyAllProfileCodePatches(const QString& profileName);

    /// Restaure tous les patchs code actifs d'un profil.
    Q_INVOKABLE QVariantMap restoreAllProfileCodePatches(const QString& profileName);

    /// Inspecte les patchs code d'un profil sans écrire, pour afficher leur état actuel.
    Q_INVOKABLE QVariantMap inspectProfileCodePatches(const QString& profileName);

    /// Sauvegarde un script auto-assembleur (texte brut) dans un profil, rejouable sans le retaper.
    Q_INVOKABLE QVariantMap saveProfileAutoAsmScript(
        const QString& profileName,
        const QString& scriptName,
        const QString& scriptText,
        const QVariantMap& metadata);

    /// Charge puis exécute un script auto-assembleur sauvegardé (même chemin que l'exécution manuelle).
    Q_INVOKABLE QVariantMap applyProfileAutoAsmScript(const QString& profileName, const QString& scriptName);

    /// Supprime un script auto-assembleur sauvegardé d'un profil.
    Q_INVOKABLE QVariantMap deleteProfileAutoAsmScript(const QString& profileName, const QString& scriptName);

    /// Scripting Lua externe : détecte un interpréteur Lua disponible et
    /// expose le chemin du helper scripts/killengine.lua. Le script pilote
    /// KillEngine via le pipe d'automatisation local, pas par injection.
    Q_INVOKABLE QVariantMap getLuaScriptingStatus() const;

    /// Exécute un script Lua dans un processus externe (lua.exe/luajit.exe).
    /// options: { luaPath?: string, timeoutMs?: int, pipeName?: string }.
    Q_INVOKABLE QVariantMap executeLuaScript(const QString& scriptText, const QVariantMap& options);

    /// Version non bloquante d'executeLuaScript, annulable via
    /// cancelLuaScriptExecution. Le résultat arrive via luaScriptExecutionFinished.
    /// Mêmes options qu'executeLuaScript.
    Q_INVOKABLE QVariantMap executeLuaScriptAsync(const QString& scriptText, const QVariantMap& options);

    /// Demande l'arrêt (kill du processus lua.exe) du script Lua externe en cours.
    Q_INVOKABLE QVariantMap cancelLuaScriptExecution();

    /// Sauvegarde un script Lua (texte brut) dans un profil, rejouable sans le retaper.
    Q_INVOKABLE QVariantMap saveProfileLuaScript(
        const QString& profileName,
        const QString& scriptName,
        const QString& scriptText,
        const QVariantMap& metadata);

    /// Supprime un script Lua sauvegardé d'un profil.
    Q_INVOKABLE QVariantMap deleteProfileLuaScript(const QString& profileName, const QString& scriptName);

    // -----------------------------------------------------------------------
    // Phase 14 — Pointer Chains (jeux modernes / applications dynamiques)
    // -----------------------------------------------------------------------

    /// Scanne la mémoire pour trouver des chaînes de pointeurs menant à une adresse cible.
    /// addressHex = adresse trouvée par scan (ex: minéraux).
    /// options keys (optionnelles): maxDepth, maxOffset, maxResults, onlyModuleBase,
    ///                              baseModules (QStringList), alignment.
    Q_INVOKABLE QVariantMap scanPointerChains(
        const QString& addressHex,
        const QVariantMap& scanOptions);

    /// Résout une chaîne de pointeurs et retourne l'adresse finale + étapes intermédiaires.
    /// chain keys: module, baseOffset (hex string), offsets (liste de hex strings).
    Q_INVOKABLE QVariantMap resolvePointerChain(const QVariantMap& chain);

    /// Sauvegarde une cible de profil avec une chaîne de pointeurs comme locator.
    Q_INVOKABLE QVariantMap savePointerChainProfileTarget(
        const QString& profileName,
        const QString& targetName,
        const QVariantMap& chain,
        const QString& valueType,
        const QString& description);

    /// Après une écriture confirmée, cherche une chaîne de pointeurs stable vers
    /// cette adresse pour qu'elle survive à un redémarrage du processus cible.
    /// Lecture seule, bornée par défaut (depth=3, offset=0x1000, results=5) pour
    /// rester rapide : à appeler explicitement, jamais automatiquement après
    /// chaque écriture (le scan de pointeurs reste l'opération la plus longue).
    /// Le meilleur candidat (`bestChain`) est directement réutilisable tel quel
    /// par `savePointerChainProfileTarget`.
    Q_INVOKABLE QVariantMap suggestStableLocatorForAddress(
        const QString& addressHex,
        const QVariantMap& options);

signals:
    void attachmentChanged();
    void scanStarted();
    void scanProgress(int percent);
    void scanStatsUpdated(int candidateCount);
    void scanFinished(const QVariantMap& result);
    void findWhatWritesFinished(const QVariantMap& result);
    void saveFileWatchFinished(const QVariantMap& result);
    void luaScriptExecutionFinished(const QVariantMap& result);
    void candidateFieldTestFinished(const QVariantMap& result);
    void pageGuardWatchFinished(const QVariantMap& result);
    void inProcessBreakpointWatchFinished(const QVariantMap& result);

    /// Émis quand un freeze par polling est détecté instable (la valeur repart
    /// avant chaque réécriture pendant plusieurs ticks d'affilée) : le
    /// classique "freeze qui clignote". Détecté automatiquement, sans que
    /// l'utilisateur ait besoin de le signaler — voir applyFreezeTick().
    void freezeInstabilityDetected(const QVariantMap& info);

    /// Émis quand une écriture confirmée (writeMemoryValueConfirmed, appelée
    /// aussi bien par un write manuel Expert que par tous les auto-write du
    /// chat Assistant) est repartie toute seule dans la fenêtre d'observation
    /// qui suit — signe que quelque chose recalcule/réécrit cette adresse.
    /// Détecté automatiquement (voir applyWriteWatchTick()) au lieu d'attendre
    /// que l'utilisateur le remarque et clique "Écrit par" à la main.
    void writeDidNotHold(const QVariantMap& info);

    /// Resultat d'une capture Find What Accesses async (kind = find_what_accesses).
    void findWhatAccessesFinished(const QVariantMap& result);
    void globalHotkeyTriggered(const QVariantMap& event);
    void aiMessage(const QString& message);
    void targetConfidenceChanged(int confidence);
    void targetFound(const QVariantMap& target);
    void processExited();
    void errorOccurred(const QString& message);

private:
    void applyFreezeTick();
    void applyWriteWatchTick();
    void registerWriteWatch(uint64_t address, killcore::ValueType type, const QByteArray& expectedBytes);
    bool restartBreakpointFreezeFromRegistry(killcore::BreakpointFreezeMode mode, QString* error = nullptr);
    QVariantMap activateBreakpointFreezeFor(
        uint64_t address,
        killcore::ValueType type,
        const QByteArray& frozenBytes,
        killcore::BreakpointFreezeMode mode,
        const QString& modeText);
    bool rememberCandidatesForUndo(QString* error = nullptr);
    void clearCandidateUndo();
    void clearCandidateValueHistory();
    void recordCandidateObservations(const QVariantList& observations);
    QVariantList candidateValueHistory(uint64_t address) const;
    void enrichSuggestedWritesWithHistory(QVariantList* suggestions) const;
    QVariantList filterAutoWriteSuggestionsByRegion(const QVariantList& suggestions, QVariantList* rejected) const;
    QVariantMap writeMemoryValueConfirmed(const QString& addressHex, const QString& valueType, const QString& value, bool persistHistory = true);
    void persistWriteHistorySequenceEntry(uint64_t address, killcore::ValueType type, const QString& valueText);
    bool hasAddressBeenWriteVerified(uint64_t address) const;
    /// Arrete au mieux les sessions de breakpoint in-process encore actives
    /// et force l'arbitre (killcore::HwBreakpointArbiter) a Idle pour
    /// l'ancienne cible (m_pid avant reassignation) -- appele au debut de
    /// attachProcess() et detachProcess(). Best-effort et non bloquant
    /// longtemps meme si la cible a deja disparu : stop() a son propre
    /// desarmement borne a 2s (voir core/debug/inprocess_breakpoint.cpp).
    void resetHardwareBreakpointStateForPreviousTarget();
    void detectStableCandidateGroup(killcore::NextScanMode mode, const QList<killcore::Candidate>& survivors, QVariantMap* result);
    QVariantMap rewriteLastAutoWriteTargets(const QString& value, const QString& query);
    QVariantMap activateChatMemoryTargetsFromQuery(const QString& query);
    QVariantMap writeChatMemoryTargetsFromQuery(const QString& query, const QString& value);
    QVariantMap freezeChatMemoryTargetsFromQuery(const QString& query, const QString& value);
    QVariantMap writeProfileTargetsFromQuery(const QString& query, const QString& value);
    QVariantMap buildFailureEscalationRecovery(const QString& query, const QStringList& numbers);
    void resetFailureEscalationState();
    QString smartSearchDebugFilePath() const;
    QString scanTelemetryFilePath() const;
    void appendSmartSearchDebug(const QString& event, const QVariantMap& payload) const;
    void appendScanTelemetry(const QString& event, const QVariantMap& payload) const;
    QString clrInspectorPipeName() const;
    QString findClrInspectorExecutable() const;
    bool ensureClrInspectorStarted(QString* error = nullptr);
    QVariantMap callClrInspectorRpc(const QString& method, const QVariantList& params, int timeoutMs = 5000);

    struct WriteRecord {
        uint64_t   address{0};
        QByteArray previousValue;
        QByteArray writtenValue;
        killcore::ValueType type{killcore::ValueType::Int32};
        QString valueText;
    };

    struct AutoWriteTarget {
        uint64_t address{0};
        killcore::ValueType type{killcore::ValueType::Int32};
    };

    // Surveillance courte apres une ecriture confirmee : combien de sondages
    // (applyWriteWatchTick) il reste avant d'arreter d'observer cette adresse
    // faute de reversion detectee (watch "reussie", rien a signaler).
    struct WriteWatchEntry {
        uint64_t address{0};
        killcore::ValueType type{killcore::ValueType::Int32};
        QByteArray expectedBytes;
        int ticksRemaining{0};
        // Mismatches consecutifs (reset a 0 des qu'un sondage matche a
        // nouveau) : exige plusieurs sondages d'affilee avant de conclure a
        // une vraie reversion, pour ne pas declencher sur un simple aleas de
        // lecture (meme principe que FreezeEntry::consecutiveDriftTicks).
        int consecutiveMismatches{0};
    };

    struct ActiveProfileTarget {
        QString profileName;
        QString targetName;
        QString groupName;
        uint64_t address{0};
        killcore::ValueType type{killcore::ValueType::Int32};
        killcore::LocatorKind locatorKind{killcore::LocatorKind::Absolute};
        QString clrTypeSubstring;
        QString clrIdentityField;
        QString clrIdentityValue;
        QString clrFieldName;
    };

    struct ActiveCodePatch {
        uint64_t address{0};
        QByteArray originalBytes;
        QByteArray patchBytes;
    };

    struct ActiveFunctionHook {
        uint64_t targetAddress{0};
        uint64_t hookFunctionAddress{0};
        uint64_t trampolineAddress{0};
        QByteArray originalBytes;
    };

    bool                    m_attached{false};
    QString                 m_processName;
    int                     m_pid{0};
    killcore::ProcessHandle m_handle;
    // Etat de blockProcessNetwork()/unblockProcessNetwork() : survit a un
    // detachProcess() pour que la regle pare-feu reste retirable meme apres
    // detach (voir doc au-dessus de la declaration Q_INVOKABLE).
    QString                 m_networkBlockRuleToken;
    QString                 m_networkBlockExePath;
    killcore::CandidateStore m_candidates;
    killcore::CandidateStore m_previousCandidates;
    QHash<uint64_t, QVariantList> m_candidateValueHistory;
    killcore::SnapshotStore  m_snapshot;
    killcore::FreezeManager  m_freeze;
    std::unique_ptr<killcore::BreakpointFreezeManager> m_breakpointFreeze;
    std::unique_ptr<killcore::GlobalHotkeyManager> m_hotkeys;
    QTimer                   m_freezeTimer;
    // Sondage independant du freeze (interval bien plus lent, pas de
    // reecriture) : surveille juste que la valeur confirmee ecrite par
    // writeMemoryValueConfirmed n'est pas repartie toute seule peu apres.
    QTimer                   m_writeWatchTimer;
    QList<WriteWatchEntry>   m_writeWatchEntries;
    QPointer<QWidget>        m_trainerOverlay;
    QPointer<QLabel>         m_trainerOverlayLabel;
    uint64_t                 m_lastWriteAddress{0};
    QByteArray               m_lastWritePreviousValue;
    QList<WriteRecord>       m_writeHistory;
    QList<AutoWriteTarget>   m_lastAutoWriteTargets;
    QList<AutoWriteTarget>   m_chatMemoryTargets;
    QHash<uint64_t, ActiveCodePatch> m_activeCodePatches;
    QHash<uint64_t, ActiveFunctionHook> m_activeFunctionHooks;
    std::optional<killcore::AutoAsmResult> m_lastAutoAsmResult;
    qint64                   m_uiInvestigationStartedMs{0};
    QList<ActiveProfileTarget> m_activeProfileTargets;
    QStringList              m_autoWriteValueHistory;
    int                      m_lastBatchStartIndex{-1};
    int                      m_lastBatchEndIndex{-1};
    // H1 (docs/STRATEGY_ROOM.md, session Solitaire du 19/08/2026) : detecte un
    // petit groupe de candidats qui reste identique sur plusieurs next scan
    // "exact" d'affilee — signal statistique de copies redondantes.
    QSet<uint64_t>           m_stableCandidateGroup;
    int                      m_stableCandidateGroupCycles{0};
    bool                     m_scanInProgress{false};
    bool                     m_hasPreviousCandidates{false};
    int                      m_nextScanRequestId{1};
    int                      m_nextDebugRequestId{1};
    bool                     m_findWhatWritesInProgress{false};
    bool                     m_findWhatAccessesInProgress{false};
    bool                     m_pageGuardWatchInProgress{false};
    std::shared_ptr<killcore::PageGuardSession> m_activePageGuardSession;
    bool                     m_inProcessBreakpointWatchInProgress{false};
    std::shared_ptr<killcore::InProcessBreakpointSession> m_activeInProcessBreakpointSession;
    std::shared_ptr<killcore::InProcessBreakpointSession> m_inProcessBreakpointFreezeSession;
    std::unique_ptr<killcore::SpeedhackSession> m_speedhackSession;
    std::unique_ptr<killcore::ApiHookSession> m_apiHookSession;
    std::shared_ptr<killcore::CancellationToken> m_activeDebugCancellation;
    std::shared_ptr<killcore::CancellationToken> m_activeScanCancellation;
    // Test automatique des champs candidats (voir testCandidateFieldsAsync) : etat
    // dedie, distinct de m_activeDebugCancellation qui est reserve aux operations
    // avec attach debugger (celle-ci ne fait que lire/ecrire de la memoire).
    bool                     m_candidateFieldTestInProgress{false};
    std::shared_ptr<killcore::CancellationToken> m_activeCandidateFieldTestCancellation;
    // PHASE 93 — surveillance fichier (watchSaveFileForChanges) : etat dedie,
    // independant des cancellations de debug/scan ci-dessus.
    bool                     m_saveFileWatchInProgress{false};
    std::shared_ptr<killcore::CancellationToken> m_activeSaveFileWatchCancellation;
    // Scripting Lua externe (executeLuaScriptAsync) : etat dedie, independant
    // des autres cancellations ci-dessus. Le token ne fait qu'armer le kill()
    // du QProcess lua.exe depuis le thread worker qui le possede -- pas de
    // manipulation cross-thread du QProcess lui-meme.
    bool                     m_luaScriptInProgress{false};
    std::shared_ptr<killcore::CancellationToken> m_activeLuaScriptCancellation;
    std::unique_ptr<QProcess>    m_clrInspectorProcess;
    int                          m_clrInspectorRequestId{1};
    killai::AIEngine         m_ai;
    bool                     m_smartSearchActive{false};
    QString                  m_smartSearchInitialValue;
    QString                  m_smartSearchTargetValue;
    QString                  m_smartSearchValueType{"Int32"};
    // Derniere valeur que l'utilisateur a rapportee comme reellement affichee
    // a l'ecran (ex: reponse a "donne-moi la nouvelle valeur observee").
    // A ne JAMAIS confondre avec m_smartSearchTargetValue (le but jamais
    // atteint) : seule m_smartSearchLastObservedValue a une chance d'exister
    // litteralement en memoire/texte, donc c'est la seule valeur valide pour
    // un traçage Trace UI string de repli.
    QString                  m_smartSearchLastObservedValue;
    // Nombre de tours de reduction (next_scan/unknown_compare) depuis le
    // dernier scan frais. Destine a terme a exiger au moins 2 tours avant
    // l'ecriture automatique (sauf confiance deja tres elevee des le
    // premier), mais PAS ENCORE CABLE : ce compteur n'est actuellement ni lu
    // ni incremente nulle part dans application_controller.cpp — l'ecriture
    // automatique ne depend pas (encore) de lui. Ne pas presumer que ce
    // garde-fou existe deja avant d'avoir verifie/implemente la logique qui
    // l'utilise.
    int                      m_smartSearchNarrowingRounds{0};
    // Palier de l'echelle de secours (ReportBadTargets) sur le lot d'adresses
    // actif : 0 = rien tente, incremente a chaque "ca n'a pas marche" pour
    // proposer une methode differente plutot que reboucler sur le meme scan.
    // Remis a 0 des qu'une nouvelle recherche ou un nouveau lot d'adresses est etabli.
    int                      m_failureEscalationLevel{0};
    // Action de l'echelle de secours dont l'assistant attend la reponse en
    // langage libre (ex: "trace_ui_string" apres avoir demande le texte
    // affiche). Sans ce suivi, une reponse texte contenant juste un nombre
    // retombe sur la regle generique d'ecriture sur les adresses actives.
    QString                  m_pendingRecoveryAction;
    // Candidats texte trouves par le dernier scanUiStrings, conserves entre
    // l'etape 1 (Tracer le texte affiche) et l'etape 2 (Filtrer + Analyser
    // sources) du pipeline Trace UI string pilote depuis le chat Assistant.
    QVariantList             m_pendingUiStringCandidates;
    // Vrai pendant les sections de startSmartSearch qui pompent
    // QCoreApplication::processEvents() en boucle bloquante pour eviter
    // qu'AppHang* ne tue la fenetre : l'appel IA generique (jusqu'a 90s,
    // pompage fait dans ai/llama_server.cpp) et l'analyse de sources
    // numeriques du pipeline Trace UI string (jusqu'a 3 rayons x 20
    // candidats de ReadProcessMemory, pompage fait localement). Sans ce
    // garde-fou, un second clic pendant l'une de ces attentes reentrerait
    // dans startSmartSearch() sur la meme pile pendant que le premier appel
    // mute encore m_candidates / m_lastAutoWriteTargets. Rejette l'appel
    // reentrant plutot que de laisser corrompre l'etat partage.
    bool                     m_smartSearchBusy{false};
};

} // namespace killengine
