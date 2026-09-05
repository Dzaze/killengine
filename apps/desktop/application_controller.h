#pragma once

#include "ai_engine.h"
#include "auto_write_state_access.h"
#include "candidates/candidate_store.h"
#include "debug/anti_debug.h"
#include "debug/breakpoint_freeze.h"
#include "debug/inprocess_breakpoint.h"
#include "debug/page_guard.h"
#include "debug/speedhack.h"
#include "inject/api_hook.h"
#include "inject/dll_injector.h"
#include "inject/dll_mask.h"
#include "inject/function_hook.h"
#include "memory/memory_reader.h"
#include "process/process_handle.h"
#include "process/process_mask.h"
#include "profiles/profile_store.h"
#include "scripting/auto_assembler.h"
#include "scan_state_access.h"
#include "snapshot/snapshot_store.h"

#include <QObject>
#include <QByteArray>
#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QPointer>

#include <memory>
#include <optional>

class QProcess;
class QWebEnginePage;

namespace killcore {
class WebView2Inspector;
class LagSwitchSession;
class HttpProxySession;
}

namespace killengine {

class AutomationPipeServer;
class AutomationPipeManager;
class ClrInspectorBridge;
class CodePatchManager;
class DebugFeatureManager;
class ExternalToolProfiler;
class FreezeHotkeyOverlayManager;
class InvestigationNotebookManager;
class KernelDriverManager;
class LuaReplManager;
class MemoryHeatmapManager;
class MemoryTimelineManager;
class PatternLearningManager;
class ProfileManager;
class SaveFileInvestigator;
class ScanningCoreManager;
class SettingsDiagnosticsManager;
class SmartSearchManager;
class SmartWatchdogManager;
class UiStringInvestigator;
class WriteFreezeCoreManager;

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
    friend class ScanningCoreManager;
    friend class ProfileManager;
    friend class SettingsDiagnosticsManager;
    friend class SmartSearchManager;

    // Propriétés exposées à QML/JS
    Q_PROPERTY(QString version READ version CONSTANT)
    Q_PROPERTY(bool   isAttached READ isAttached NOTIFY attachmentChanged)
    Q_PROPERTY(QString processName READ processName NOTIFY attachmentChanged)

public:
    explicit ApplicationController(QObject* parent = nullptr);
    ~ApplicationController() override;

    /// Câblage interne (appelé une fois par main.cpp après création de la
    /// QWebEnginePage) : nécessaire à callVueStoreAction() ci-dessous pour
    /// injecter du JS dans la page. Pas Q_INVOKABLE : ne doit jamais être
    /// rebranchable depuis le pipe d'automatisation.
    void setWebEnginePage(QWebEnginePage* page);

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

    /// UWP-STATE-1 — Compare deux snapshots obtenus via discoverProcessSaveFiles
    /// (typiquement un avant/après une action utilisateur comme gagner de
    /// l'XP) et classe les fichiers en ajoutés/supprimés/modifiés (taille et
    /// date de dernière écriture), sans aucun accès disque supplémentaire —
    /// comparaison pure sur les listes déjà obtenues. Voir
    /// docs/UWP_STATE_INSPECTOR_SPEC.md. Retourne
    /// {success, added, removed, modified, addedCount, removedCount,
    /// modifiedCount, unchangedCount, error}.
    Q_INVOKABLE QVariantMap compareProcessSaveFileSnapshots(const QVariantList& before, const QVariantList& after) const;

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

    /// Déduit l'espacement entre deux instances depuis un champ homologue.
    Q_INVOKABLE QVariantMap inferStructureInstanceDelta(
        const QString& baseAddressAHex,
        const QString& fieldAddressAHex,
        const QString& baseAddressBHex,
        const QString& fieldAddressBHex,
        const QVariantMap& options) const;

    /// Scanne automatiquement la mémoire pour trouver toutes les instances d'un template de structure.
    Q_INVOKABLE QVariantMap findStructureInstances(const QVariantMap& templateJson) const;

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

    /// PHASE 250 (SC2 solarite) : session changed-pages multi-rounds avec
    /// consensus déterministe. Les blocs capturés roulent d'un round à
    /// l'autre (baseline roll-forward), chaque transition affichée
    /// (140 -> 135 -> 130...) alimente un accumulateur qui élimine les
    /// adresses contredites/stales et classe celles qui suivent chaque
    /// transition — l'intersection multi-rounds isole la source gameplay
    /// des copies UI volatiles. Voir display_string_investigator.h.
    Q_INVOKABLE QVariantMap startChangedPagesSession(const QVariantMap& options);
    Q_INVOKABLE QVariantMap applyChangedPagesRound(
        const QString& previousValue,
        const QString& currentValue,
        const QVariantMap& options);
    Q_INVOKABLE QVariantMap getChangedPagesConsensus(const QVariantMap& options) const;
    Q_INVOKABLE QVariantMap stopChangedPagesSession();

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

    /// Compare le snapshot unknown initial avec l'état courant. `deltaValue`
    /// (optionnel, requis pour mode "delta") : delta AFFICHÉ observé entre
    /// deux lectures (ex. "7") — SC2-UNKNOWN-1, testé automatiquement contre
    /// les mêmes échelles fixed-point que le scan exact (x10/x100/x1000/
    /// x4096/x65536) en type "Auto", labellisant les survivants en
    /// conséquence (ex. "Int32 x4096") pour que le narrowing exact suivant
    /// sache déjà retraduire une valeur affichée dans la bonne échelle.
    Q_INVOKABLE QVariantMap unknownNextScan(const QString& mode, const QString& valueType, const QString& deltaValue = QString());

    /// Compare le snapshot unknown dans un worker thread. Voir unknownNextScan
    /// pour `deltaValue`.
    Q_INVOKABLE QVariantMap unknownNextScanAsync(const QString& mode, const QString& valueType, const QString& deltaValue = QString());

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

    /// PHASE 130 : classifie une adresse candidate comme "probablement un champ
    /// affiché recalculé" vs "probablement une source événementielle", à partir
    /// d'une capture findWhatWrites passive (core/scanner/display_source_classifier.*).
    /// Lecture seule -- aucune écriture n'est jamais faite sur la cible.
    /// options keys : size (1/2/4/8), captureWindowMs (défaut 800), maxHits (défaut 12)
    Q_INVOKABLE QVariantMap analyzeFieldStability(const QString& addressHex, const QVariantMap& options);

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

    /// Valide la stabilité d'une page mémoire avant d'armer un Page Guard.
    /// Lit la page N fois avec un intervalle configurable et retourne si la page est stable.
    Q_INVOKABLE QVariantMap validatePageStability(const QString& addressHex, const QVariantMap& options);

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

    /// EXTMOD-1 : capture un checkpoint nommé (modules + carte mémoire + hash de
    /// contenu optionnel) de l'état structurel du process attaché. Options :
    /// moduleName (filtre les régions à un module précis), hashContent (bool,
    /// défaut true), maxHashBytesMb (défaut 64, clamp 8-512).
    Q_INVOKABLE QVariantMap captureProfilerCheckpoint(const QString& label, const QVariantMap& options);

    /// EXTMOD-1/EXTMOD-2 : diff entre deux checkpoints déjà capturés —
    /// modules ajoutés/retirés, régions ajoutées/retirées/changées avec
    /// classification (injected_module_page, new_executable_writable_page,
    /// code_page_changed, ...), et depuis EXTMOD-2 un diff fin par pages 4K
    /// à l'intérieur de chaque région changée (options : includePageDiff
    /// défaut true, maxSampleDeltasPerPage défaut 16, maxTopChangedPages
    /// défaut 100) exposé aussi en agrégat trié dans "topChangedPages".
    Q_INVOKABLE QVariantMap getProfilerDiff(const QString& labelA, const QString& labelB, const QVariantMap& options) const;

    /// EXTMOD-1 : liste les checkpoints capturés dans la session courante.
    Q_INVOKABLE QVariantMap listProfilerCheckpoints() const;

    /// EXTMOD-1 : vide les checkpoints de la session courante (aussi fait
    /// automatiquement à chaque nouvel attachProcess).
    Q_INVOKABLE QVariantMap clearProfilerSession();

    /// EXTMOD-2 : capture une étape timeline nommée (ex: "baseline",
    /// "tool_attached_off", "toggle_on", "stimulus_done", "toggle_off") —
    /// mêmes options que captureProfilerCheckpoint (moduleName,
    /// maxHashBytesMb, maxPageBytesMb) — et alimente le tracker de stabilité
    /// par page utilisé par getProfilerTimelineSummary.
    Q_INVOKABLE QVariantMap recordProfilerTimelineStep(const QString& stepName, const QVariantMap& options);

    /// EXTMOD-2 : résumé de la session timeline — pages classées
    /// toggle_state_candidate (change plus d'une fois mais pas à chaque
    /// étape — meilleur signal), runtime_noise (change à chaque étape,
    /// probablement un compteur), one_time_state_change (change une seule
    /// fois, ex: init d'un module injecté).
    Q_INVOKABLE QVariantMap getProfilerTimelineSummary() const;

    /// EXTMOD-2 : vide la session timeline (les checkpoints eux-mêmes,
    /// capturés via clearProfilerSession, ne sont pas affectés).
    Q_INVOKABLE QVariantMap clearProfilerTimeline();

    /// Cherche une signature AOB dans les régions mémoire du processus.
    /// Pattern: "48 8B ?? ?? 89", options: executableOnly, imageOnly, startAddress, stopAddress, maxResults.
    Q_INVOKABLE QVariantMap scanAobPattern(const QString& pattern, const QVariantMap& options);

    /// Génère une signature AOB exacte à partir des octets autour d'une adresse d'instruction.
    /// options keys: beforeBytes, length.
    /// ATTENTION (constaté en direct le 29/08/2026, docs/INVESTIGATION_PLAYBOOK.md symptôme 5) : ne jamais appeler
    /// ceci directement sur le `instructionPointer` brut d'un hit findWhatWrites -- un breakpoint matériel piège
    /// APRÈS l'exécution de l'instruction, donc ce RIP pointe sur l'instruction SUIVANTE (souvent un `ret`, un saut,
    /// ou une instruction sans rapport avec l'écriture), pas sur l'écriture elle-même. Toujours appeler
    /// disassembleBackward() sur ce RIP d'abord pour localiser la vraie instruction (catégorie "memory-write" dans
    /// le résultat), puis générer l'AOB sur CETTE adresse.
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

    /// Ecriture RiskGate chat (29/08/2026) : points d'entree publics, appeles
    /// uniquement APRES un clic explicite sur le recoveryAction correspondant
    /// renvoye par startSmartSearch quand l'intention WriteMemoryTargets/
    /// FreezeMemoryTargets/RewriteLastTargets est 100% originaire du chat
    /// (AutoWriteTarget::chatOrigin) -- avant, ces ecritures s'executaient
    /// sans AUCUNE interaction utilisateur (bug reel constate en direct
    /// pendant PHASE 120-D). Le clic sur le bouton du chat EST la
    /// confirmation (frontend n'ouvre plus de second modal confirmRiskAction
    /// ici, retire a la demande explicite du proprietaire -- juge redondant
    /// avec la carte chat qui affiche deja l'avertissement de risque, et
    /// l'utilisateur a deja tape l'adresse ET la valeur explicitement avant
    /// d'arriver ici). N'ajoutent aucune logique d'ecriture : appellent
    /// directement les fonctions privees deja existantes
    /// (writeChatMemoryTargetsFromQuery/freezeChatMemoryTargetsFromQuery/
    /// rewriteLastAutoWriteTargets), qui elles n'ont jamais change.
    Q_INVOKABLE QVariantMap confirmChatMemoryWrite(const QString& value);
    Q_INVOKABLE QVariantMap confirmChatMemoryFreeze(const QString& value);
    Q_INVOKABLE QVariantMap confirmRewriteLastAutoWrite(const QString& value);

    /// Lance une auto-résolution prudente : plan IA + premières actions sûres seulement.
    Q_INVOKABLE QVariantMap startAutoResolve(const QString& query, const QVariantMap& options);

    /// Résume le contexte d'enquête et la télémétrie récente pour guider l'IA proactive.
    Q_INVOKABLE QVariantMap getAutoResolveReport(int maxEvents) const;

    /// Vide la mémoire locale d'auto-résolution (QSettings) pour le processus courant ou tous les processus.
    Q_INVOKABLE QVariantMap clearAutoResolveMemory(bool allProcesses);

    /// Carnet d'hypothèses (PHASE 120-E/F) : moteur de pondération déterministe,
    /// aucune génération d'hypothèse ni de "prochaine expérience" ici (PHASE 120-G).
    Q_INVOKABLE QVariantMap addInvestigationHypothesis(const QString& description, int baselineScore = 50);
    Q_INVOKABLE QVariantMap recordInvestigationTestResult(const QString& hypothesisId, bool confirmed, const QString& evidenceNote);
    Q_INVOKABLE QVariantMap getInvestigationNotebookSynthesis() const;
    Q_INVOKABLE QVariantMap proposeInvestigationNotebookPlan(const QString& symptom, const QVariantMap& options);
    Q_INVOKABLE QVariantMap resetInvestigationNotebook();

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

    /// Liste les connexions TCP/UDP actives du processus attaché (lecture
    /// seule, via GetExtendedTcpTable/GetExtendedUdpTable). Inclut la
    /// résolution DNS best-effort (getnameinfo, asynchrone, cache LRU).
    /// Retourne {success, connections: [{protocol, localAddr, remoteAddr,
    /// remoteHost, state, pid}], error}.
    Q_INVOKABLE QVariantMap getProcessNetworkConnections();

    /// Liste les modules DLL réseau chargés par le processus attaché
    /// (EnumProcessModules + GetModuleFileNameEx, filtré sur une liste
    /// connue de DLL réseau). Retourne {success, modules: [{name, path,
    /// category, description}], error}.
    Q_INVOKABLE QVariantMap getProcessNetworkModules();

    /// Proxy HTTP : intercepte les requêtes HTTP/HTTPS du process attaché
    /// via injection DLL + hook WinINet/WinHTTP.
    Q_INVOKABLE QVariantMap startHttpProxy(int port, bool interceptHttps);
    Q_INVOKABLE QVariantMap stopHttpProxy();
    Q_INVOKABLE QVariantMap getHttpProxyRequests();
    Q_INVOKABLE QVariantMap modifyHttpRequest(const QString& requestId, const QString& newRequestBody);

    /// Spoof DNS : ajoute/retire une entrée dans le fichier hosts Windows.
    Q_INVOKABLE QVariantMap spoofDns(const QString& domain, const QString& targetIp);
    Q_INVOKABLE QVariantMap restoreDns(const QString& domain);

    /// Lag switch : retarde les fonctions recv/WSARecv du process attaché
    /// via injection DLL + MinHook.
    Q_INVOKABLE QVariantMap setLagSwitch(bool enabled, int delayMs);

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

    /// Inspection WebView2/JS via CDP. Ces methodes exposent seulement une
    /// facade QWebChannel autour de killcore::WebView2Inspector : discovery
    /// direct/WDP, connexion a une target par PID, lectures DOM et evaluation
    /// JS. Les retours portent risk/requiresConfirmation pour que le RiskGate
    /// frontend confirme explicitement les actions sensibles (connexion a un
    /// process externe, JS arbitraire).
    Q_INVOKABLE QVariantMap getWebView2InspectorStatus() const;
    Q_INVOKABLE QVariantMap listWebView2CdpTargets(int browserProcessId, const QVariantMap& options) const;
    Q_INVOKABLE QVariantMap connectWebView2Inspector(int browserProcessId, const QVariantMap& options);
    Q_INVOKABLE QVariantMap disconnectWebView2Inspector();
    Q_INVOKABLE QVariantMap evaluateWebView2JavaScript(const QString& expression, const QVariantMap& options);
    Q_INVOKABLE QVariantMap findWebView2DisplayedValues(const QString& value, const QVariantMap& options);
    Q_INVOKABLE QVariantMap findWebView2DisplayedText(const QString& text, const QVariantMap& options);

    /// WEBVIEW-F — Sonde le scope global JS de la target WebView2 connectee
    /// (killcore::WebView2Inspector::probeGlobalScope()) et isole les
    /// globales *ajoutees par la page* (variables de jeu, SDK publicitaires)
    /// des globales natives Chromium. Baseline dynamique par defaut : ouvre
    /// une connexion CDP jetable et independante (pas m_webView2Inspector,
    /// qui reste sur la target de l'utilisateur pendant tout le sondage) vers
    /// une target about:blank du meme host pour connaitre les globales
    /// natives de cette version precise de Chromium ; repli sur une petite
    /// liste statique si aucune target about:blank n'est disponible. Ajoute
    /// aussi un aperçu structure (compte <video>/<audio>, srcs <iframe>).
    /// Lecture seule. Retourne {success, customGlobals, totalGlobalsSeen,
    /// baselineMode, media, target, error}.
    Q_INVOKABLE QVariantMap probeWebView2GlobalScope();

    /// Active/désactive la variable d'environnement utilisateur
    /// WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS=--remote-debugging-port=9333 (via
    /// HKCU\Environment + broadcast WM_SETTINGCHANGE, équivalent de `setx` en
    /// natif). Force TOUS les hôtes WebView2 du user Windows courant à exposer
    /// un port de debug CDP à leur prochain lancement — portée large et
    /// persistante, jamais posée silencieusement : le frontend confirme via
    /// RiskGate avant d'appeler enable. disable retire la variable sans
    /// confirmation (symétrie avec disableAutomationMode()).
    Q_INVOKABLE QVariantMap enableWebView2CdpDebugFlag();
    Q_INVOKABLE QVariantMap disableWebView2CdpDebugFlag();
    Q_INVOKABLE QVariantMap getWebView2CdpDebugFlagStatus() const;

    /// Diagnostic système pour préparer l'inspection WebView2 sur les cibles
    /// UWP/Store (chaîne Windows Device Portal, cf. docs/PHASE_TRACKER.md) :
    /// état Developer Mode (HKLM\...\AppModelUnlock) et de la capability
    /// optionnelle Tools.DeveloperMode.Core. Lecture seule.
    Q_INVOKABLE QVariantMap getWebView2SystemPrepStatus() const;

    /// Installe la capability Windows optionnelle Tools.DeveloperMode.Core
    /// (Add-WindowsCapability, invite UAC visible via `runas` — jamais
    /// silencieux, même mécanisme que requestWindowsDefenderExclusion()).
    /// Peut prendre plusieurs minutes et rester silencieuse côté process :
    /// le frontend doit prévenir l'utilisateur avant de lancer, puis proposer
    /// un nouveau getWebView2SystemPrepStatus() pour re-tester. N'ouvre PAS le
    /// CDP TCP direct des apps Store UWP (restriction AppContainer séparée,
    /// toujours en place) : uniquement un prérequis pour Windows Device Portal.
    Q_INVOKABLE QVariantMap installWebView2DeveloperModeCapability();

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

    /// Masque (hide=true) ou restaure (hide=false) un handle specifique dans
    /// la table de handles du process proprietaire via le driver noyau
    /// (kKillEngineKernelIoctlHandleTable). ownerPid vide = process courant
    /// (KillEngine lui-meme). handleValue en hexadecimal (ex: "0x1234).
    /// Necessite capabilities.handleTable=true (voir probeKernelDriver).
    Q_INVOKABLE QVariantMap handleTable(const QString& ownerPid, const QString& handleValue, bool hide) const;

    /// Retourne un diagnostic lisible du runtime IA local (modèle GGUF + llama-cli).
    Q_INVOKABLE QVariantMap getAiModelStatus() const;

    /// Ouvre un sélecteur de fichier natif pour choisir un modèle GGUF (remplace la saisie manuelle du chemin).
    Q_INVOKABLE QVariantMap browseForModelFile();

    /// Catalogue des modules complémentaires optionnels (runtime Lua externe,
    /// modèle IA GGUF, inspecteur CLR, driver noyau) : statut installé/manquant,
    /// description, script d'installation. Alimente la vue "Modules" du menu
    /// gauche — finalité d'exportabilité sur d'autres machines (téléchargement
    /// des dépendances depuis l'UI, demande utilisateur).
    Q_INVOKABLE QVariantMap getModuleCatalog() const;

    /// Lance l'installation d'un module du catalogue (moduleId = lua_runtime |
    /// ai_model | clr_inspector | kernel_driver) sans bloquer l'UI : les trois
    /// premiers modules tournent dans un thread worker (PowerShell), kernel_driver
    /// passe par une invite UAC visible (runas, même mécanisme que
    /// installWebView2DeveloperModeCapability). Progression via
    /// moduleInstallProgress, résultat final via moduleInstallFinished (payload
    /// porte requestId + moduleId).
    Q_INVOKABLE QVariantMap installModule(const QString& moduleId, const QVariantMap& options);

    /// Annule l'installation de module en cours (thread worker uniquement —
    /// pas d'effet sur le cas élevé UAC déjà détaché).
    Q_INVOKABLE QVariantMap cancelModuleInstall();

    // MODULES-V2 : Environnement de test + Sécurité/Stealth
    /// Vérifie si l'EDR bloque l'injection de code sur le process attaché
    /// (VirtualAllocEx + WriteProcessMemory + CreateRemoteThread simulés).
    Q_INVOKABLE QVariantMap checkEdrBlocking() const;

    /// Ajoute une exclusion Defender pour le dossier build/bin (PowerShell admin).
    Q_INVOKABLE QVariantMap addEdrExclusion(const QString& path);

    /// Active (disabled=true) ou réactive (disabled=false) Windows Defender via
    /// le registre (élévation UAC) — alternative plus agressive à l'exclusion
    /// ciblée, réversible en rappelant avec disabled=false.
    Q_INVOKABLE QVariantMap setWindowsDefenderDisabled(bool disabled);

    /// Active/désactive uniquement la surveillance comportementale Defender
    /// (élévation UAC, réversible) — moins agressif que la désactivation complète.
    Q_INVOKABLE QVariantMap setDefenderBehaviorMonitoringDisabled(bool disabled);

    /// Vérifie si le privilège SeDebugName est actif pour le process courant.
    Q_INVOKABLE QVariantMap checkDebugPrivilege() const;

    /// Active le privilège SeDebugName pour le process courant.
    Q_INVOKABLE QVariantMap enableDebugPrivilege();

    /// Applique un profil stealth (sc2/default/minimal) sur le process attaché.
    Q_INVOKABLE QVariantMap applyStealthProfile(const QString& profile);

    /// Restaure le mode stealth actif.
    Q_INVOKABLE QVariantMap restoreStealthProfile();

    /// Masque un handle spécifique dans la table de handles de la cible (via driver kernel).
    Q_INVOKABLE QVariantMap hideHandle(uint64_t ownerPid, uint64_t handleValue);

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

    /// Exporte les cibles pointer_chain d'un profil en JSON partageable.
    Q_INVOKABLE QVariantMap exportPointerMap(const QString& profileName);

    /// Importe/fusionne une pointer map JSON dans un profil existant ou nouveau.
    /// options keys: replaceExisting (défaut false).
    Q_INVOKABLE QVariantMap importPointerMap(
        const QString& profileName,
        const QString& pointerMapJson,
        const QVariantMap& options);

    /// Met à jour les dépendances Trainer persistées dans une cible de profil.
    /// dependencyNames contient des noms de cibles, pas des ids frontend locaux.
    Q_INVOKABLE QVariantMap setProfileTargetDependencies(
        const QString& profileName,
        const QString& targetName,
        const QVariantList& dependencyNames);

    /// Exporte les artefacts runtime du profil vers Ghidra : JSON + script Python Ghidra.
    Q_INVOKABLE QVariantMap exportGhidraArtifacts(const QString& profileName);

    /// Importe des symboles Ghidra JSON/CSV et enrichit les cibles/patchs du profil.
    Q_INVOKABLE QVariantMap importGhidraSymbols(const QString& profileName, const QString& symbolsText);

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

    /// PROPOSITIONS-1 #5 — Memory Heatmap : démarre la collecte d'activité
    /// mémoire (lectures/écritures) sur le process attaché. Le heatmap
    /// visualise l'intensité d'accès mémoire pour identifier les zones
    /// "chaudes" où le jeu modifie constamment des valeurs. Câblage +
    /// correction du collecteur (perf + intensité) : 02/09/2026, Claude —
    /// voir PHASE_TRACKER.md "ANALYSE-CLINE-1" et
    /// core/visualization/memory_heatmap_collector.cpp.
    /// `addressHex` (optionnel) : borne le scan à partir de cette adresse
    /// (remplit options.minAddress) — laisser vide pour tout l'espace
    /// process (le collecteur borne lui-même le travail par tick, voir
    /// HeatmapConfig::maxPagesPerTick, donc c'est sûr même sans borne).
    /// options : minAddress/maxAddress (hex ou décimal), regionSize (défaut
    /// 4096), maxRegions (défaut 10000), samplingIntervalMs (défaut 100),
    /// trackReads/trackWrites (défaut true), maxPagesPerTick (défaut 4096).
    Q_INVOKABLE QVariantMap startMemoryHeatmap(const QString& addressHex, const QVariantMap& options);

    /// Arrête la collecte du heatmap et retourne les données collectées
    /// (stats + régions les plus actives).
    Q_INVOKABLE QVariantMap stopMemoryHeatmap();

    /// Retourne l'état courant du heatmap (actif/inactif, statistiques).
    Q_INVOKABLE QVariantMap getMemoryHeatmapStatus() const;

    /// Retourne les données du heatmap pour affichage (top régions +
    /// statistiques). Peut être appelé pendant la collecte pour un
    /// affichage temps réel.
    Q_INVOKABLE QVariantMap getMemoryHeatmapData() const;

    /// PROPOSITIONS-1 #3 — Memory Timeline : surveille l'évolution d'un
    /// ensemble d'adresses explicitement ajoutées (contrairement au Heatmap,
    /// ne scanne jamais tout l'espace mémoire — sûr par construction).
    /// Câblage backend + frontend initial : 02/09/2026, Claude — voir
    /// docs/PHASE_TRACKER.md "Memory Timeline". L'analyse avancée est
    /// portée par MemoryTimelineAnalyzer (patterns/corrélations/profil/
    /// prédiction/rapport).
    Q_INVOKABLE QVariantMap addTimelineAddress(const QString& addressHex, int valueSize);
    Q_INVOKABLE QVariantMap removeTimelineAddress(const QString& addressHex);
    Q_INVOKABLE QVariantMap clearTimelineAddresses();
    Q_INVOKABLE QVariantMap getTimelineWatchedAddresses() const;

    /// options : samplingIntervalMs, maxDurationMs, trackOnlyChanges.
    Q_INVOKABLE QVariantMap setTimelineConfig(const QVariantMap& options);
    Q_INVOKABLE QVariantMap getTimelineConfig() const;

    /// Démarre/arrête la collecte sur le process attaché. Il faut au moins
    /// une adresse surveillée (addTimelineAddress) avant de démarrer.
    Q_INVOKABLE QVariantMap startTimelineCollection();
    Q_INVOKABLE QVariantMap stopTimelineCollection();
    Q_INVOKABLE QVariantMap getTimelineStatus() const;

    Q_INVOKABLE QVariantMap getTimelineSeriesForAddress(const QString& addressHex) const;
    Q_INVOKABLE QVariantMap getAllTimelineSeries() const;
    Q_INVOKABLE QVariantMap findVolatileTimelineAddresses(double threshold);
    Q_INVOKABLE QVariantMap findStableTimelineAddresses(int minDurationMs);

    /// Exporte vers Documents/KillEngine/timeline/ (chemin auto-généré,
    /// horodaté) et retourne le chemin utilisé — même convention que
    /// dumpMemoryRegion.
    Q_INVOKABLE QVariantMap exportTimelineToJson();
    Q_INVOKABLE QVariantMap exportTimelineToCsv();

    /// Analyses avancées de la timeline collectée.
    Q_INVOKABLE QVariantMap detectTimelinePatterns(const QString& addressHex);
    Q_INVOKABLE QVariantMap analyzeTimelineBehavior(const QString& addressHex);
    Q_INVOKABLE QVariantMap predictTimelineNextValue(const QString& addressHex);

    /// Corrélations entre toutes les séries surveillées actuellement (pas de
    /// paramètre d'adresse — compare toutes les paires). Rapport texte
    /// résumant patterns/corrélations/profils sur l'ensemble des séries.
    /// Ajoutés le 03/09/2026 (Claude) : la logique existait déjà sur
    /// MemoryTimelineManager depuis le câblage initial mais n'était pas
    /// exposée ici ni côté frontend.
    Q_INVOKABLE QVariantMap findTimelineCorrelations();
    Q_INVOKABLE QVariantMap generateTimelineReport();

    /// PROPOSITIONS-1 #2 — Pattern Learning : classification de patterns
    /// mémoire (compteur/santé/flag/timer/coordonnée), détection de moteur
    /// de jeu (Unity/Unreal/Godot), profils par jeu réutilisables entre
    /// sessions. Câblage + bugs réels trouvés/corrigés (SQLite absent du
    /// projet → réécrit en JSON, fonction jamais définie, ordre de
    /// déclaration, const-correctness) : 02/09/2026, Claude — voir
    /// docs/PHASE_TRACKER.md "ANALYSE-CLINE-1"/"Pattern Learning".
    /// Initialisé automatiquement à la construction (ouvre juste un fichier
    /// JSON, pas besoin de process attaché) — pas de vue Vue.js dédiée,
    /// même statut que Memory Heatmap.
    Q_INVOKABLE bool isPatternLearningInitialized() const;
    Q_INVOKABLE QVariantMap getPatternLearningStatistics() const;

    /// moduleNames: liste de noms de modules chargés. memorySample: {data: <bytes>}.
    Q_INVOKABLE QVariantMap detectGameEngine(const QVariantList& moduleNames, const QVariantMap& memorySample);

    /// Classe un historique de valeurs observées à une adresse (compteur, santé, flag, timer...).
    Q_INVOKABLE QVariantMap classifyMemoryPattern(const QString& addressHex, const QVariantList& valueHistory, const QVariantList& timestamps);

    Q_INVOKABLE QVariantMap loadGameProfile(const QString& gameName);
    Q_INVOKABLE bool saveGameProfile(const QVariantMap& profile);
    Q_INVOKABLE QVariantList listKnownGameProfiles();
    Q_INVOKABLE bool deleteGameProfile(const QString& gameName);
    Q_INVOKABLE void recordLearningSession(const QVariantMap& session);
    Q_INVOKABLE QVariantList suggestPatternResolutionPaths(const QString& gameName, int targetType);
    Q_INVOKABLE QVariantList suggestPatternValueTypes(int engineType, int patternType);
    Q_INVOKABLE double getPatternValueTypeSuccessRate(const QString& gameName, const QString& valueType);
    Q_INVOKABLE QVariantList clusterPatternAddresses(const QVariantList& addresses, const QVariantList& features);

    /// Suivi temps réel : accumule des échantillons pour une adresse puis
    /// classe le pattern une fois assez de données collectées.
    Q_INVOKABLE void startPatternTracking(const QString& addressHex, const QString& valueType);
    Q_INVOKABLE void stopPatternTracking(const QString& addressHex);
    Q_INVOKABLE void recordPatternTrackingValue(const QString& addressHex, double value);
    Q_INVOKABLE QVariantMap getPatternTrackingAnalysis(const QString& addressHex);

    /// candidates: liste de {address, history}. Classe chaque candidat et regroupe par type dominant.
    Q_INVOKABLE QVariantMap analyzePatternCandidates(const QVariantList& candidates);
    Q_INVOKABLE QVariantList getTopPatternSuggestions(const QString& gameName, int patternType, int count);

    /// PROPOSITIONS-1 #4 — Live Lua REPL : démarre un process lua.exe
    /// persistant (scripts/killengine_repl_driver.lua) gardé vivant entre
    /// chaque ligne envoyée via sendLuaReplLine, contrairement à
    /// executeLuaScript qui relance un process à chaque appel — les
    /// variables et `require("killengine")` persistent d'une ligne à
    /// l'autre. Options : luaPath?, timeoutMs? (par ligne, défaut 15000),
    /// pipeName?.
    Q_INVOKABLE QVariantMap startLuaRepl(const QVariantMap& options);

    /// Envoie une ligne au REPL démarré via startLuaRepl. Toujours
    /// asynchrone (comme executeLuaScriptAsync) : retourne {started:true,
    /// requestId} immédiatement — une ligne qui appelle ke.call(...) doit
    /// pouvoir joindre le pipe d'automatisation sans que ce thread soit
    /// bloqué à l'attendre. Résultat réel via getLuaReplLineResult(requestId)
    /// (poll, sûr sur le pipe d'automatisation) ou le signal luaReplLineFinished
    /// (frontend Qt uniquement, le pipe ne relaie pas les signaux).
    Q_INVOKABLE QVariantMap sendLuaReplLine(const QString& line);

    /// Résultat d'une ligne envoyée via sendLuaReplLine (poll par requestId).
    Q_INVOKABLE QVariantMap getLuaReplLineResult(int requestId) const;

    /// Historique de la session REPL courante (0 ou négatif = tout).
    Q_INVOKABLE QVariantMap getLuaReplHistory(int maxEntries) const;

    /// Complétion des fonctions `ke.*` connues (extraites de
    /// scripts/killengine.lua), filtrées par `prefix` (vide = tout).
    Q_INVOKABLE QVariantMap getLuaReplCompletions(const QString& prefix) const;

    /// Arrête le REPL (le process lua.exe persistant).
    Q_INVOKABLE QVariantMap stopLuaRepl();

    /// Statut courant du REPL (actif, lignes en attente, taille historique).
    Q_INVOKABLE QVariantMap getLuaReplStatus() const;

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

    // -----------------------------------------------------------------------
    // PHASE 119 — Pont pipe d'automatisation -> couche Vue/Pinia
    // (docs/POWER_UP_ROADMAP.md section N, decide priorite par le
    // proprietaire le 25/08/2026)
    // -----------------------------------------------------------------------

    /// Invoque une action de store Pinia (ui/src/stores/app.ts) depuis le
    /// pipe d'automatisation, pour les actions purement frontend qui n'ont
    /// aucun miroir Q_INVOKABLE cote C++ (ex: keepCandidate/ignoreCandidate/
    /// addAddressToWatch). Contrairement au reste de cette classe, ceci
    /// n'execute AUCUNE logique C++ : construit un appel JSON-safe et
    /// l'injecte via QWebEnginePage::runJavaScript() vers
    /// window.__killengineAutomationBridge.dispatch(action, args), une
    /// surface JS explicitement bornee cote store (voir app.ts). Double
    /// liste blanche par conception (defense en profondeur, jamais un
    /// pont "execute ce JS arbitraire") : `action` doit d'abord matcher la
    /// liste blanche C++ interne (kAllowedVueStoreActions) AVANT meme que le
    /// JS soit construit, PUIS le bridge JS revalide `action` de son cote
    /// contre sa propre table de fonctions -- les deux listes doivent
    /// matcher independamment pour qu'un appel aboutisse. `args` est un
    /// tableau positionnel serialise en JSON via QJsonDocument (jamais de
    /// concatenation de string), passe tel quel a la fonction de store
    /// ciblee. Bloquant (QEventLoop + timeout 5s), comme findWhatWrites vs
    /// findWhatWritesAsync : le pipe ne peut pas relire le resultat d'un
    /// callback Qt asynchrone. Retourne {success, action, result, error}.
    Q_INVOKABLE QVariantMap callVueStoreAction(const QString& action, const QVariantList& args);

    /// Mode Automation (Settings) : active le pipe d'automatisation local de
    /// façon persistante (QSettings "automation/pipeEnabled"), en plus du
    /// chemin dev existant (variable d'environnement KILLENGINE_AUTOMATION_PIPE=1,
    /// inchangé). Ne change PAS le comportement du pipe lui-même (bypass
    /// RiskGate par conception, voir automation_pipe_server.h) -- remplace
    /// seulement la façon de l'activer, pour que ce soit un vrai toggle
    /// produit plutôt qu'une variable d'environnement cachée. Démarre le
    /// pipe immédiatement si pas déjà actif.
    Q_INVOKABLE QVariantMap enableAutomationMode();

    /// Symétrique de enableAutomationMode() : dépersiste le réglage et ferme
    /// le pipe s'il tourne. Pas de confirmation nécessaire côté frontend --
    /// désactiver un accès n'est jamais une action à risque.
    Q_INVOKABLE QVariantMap disableAutomationMode();

    /// Statut affiché dans le panneau Settings "Mode Automation" : actif,
    /// nom du pipe, et compteur d'activité (voir AutomationPipeServer::status()).
    Q_INVOKABLE QVariantMap getAutomationPipeStatus();

    /// Active un mode de protection unifié (anti-debug, masquage processus, masquage DLL).
    /// profile: "sc2" (tout actif), "default" (anti-debug seul), "minimal" (masquage processus seul).
    Q_INVOKABLE QVariantMap applyStealthMode(const QString& profile);

    /// Désactive le mode de protection unifié et restaure l'état original.
    Q_INVOKABLE QVariantMap restoreStealthMode();

    /// Retourne l'état actuel du mode de protection unifié.
    Q_INVOKABLE QVariantMap getStealthStatus() const;

    /// Stealth Profiler (chantier Cline, repris 03/09/2026) — analyse le
    /// process attaché (modules chargés + visibilité du débogueur) et
    /// retourne un score de détectabilité (0-100) avec des recommandations
    /// concrètes, au lieu du choix binaire de profil fixe qu'est
    /// applyStealthMode(). Logique de scoring pure dans
    /// core/debug/stealth_profiler.h, cet appel ne fait que rassembler les
    /// entrées réelles (ProcessEnumerator::enumerateModules,
    /// CheckRemoteDebuggerPresent).
    Q_INVOKABLE QVariantMap analyzeStealthRisk() const;

    /// Démarre AutomationPipeServer si KILLENGINE_AUTOMATION_PIPE=1 (chemin dev
    /// existant) OU si QSettings "automation/pipeEnabled" est vrai (mode
    /// Automation persistant). Appelée une fois au démarrage depuis main.cpp
    /// (remplace l'ancien bloc inline) ET depuis enableAutomationMode() pour
    /// le cas où le toggle est activé en cours de session. Pas un Q_INVOKABLE :
    /// appelée en C++ direct depuis main.cpp, pas depuis le frontend.
    /// Délègue à AutomationPipeManager (candidat C11, docs/REFACTOR_ROADMAP.md,
    /// extrait le 29/08/2026) -- ces 4 méthodes restent ici comme façades
    /// minces, le cycle de vie réel vit dans apps/desktop/automation_pipe_manager.*.
    void ensureAutomationPipeStartedIfConfigured();

signals:
    void attachmentChanged();
    void scanStarted();
    void scanProgress(int percent);
    void scanStatsUpdated(int candidateCount);
    void scanFinished(const QVariantMap& result);
    void findWhatWritesFinished(const QVariantMap& result);
    void saveFileWatchFinished(const QVariantMap& result);
    void luaScriptExecutionFinished(const QVariantMap& result);
    /// Vue "Modules" : progression d'une installation de module en cours
    /// (payload : requestId, moduleId, percent, message).
    void moduleInstallProgress(const QVariantMap& progress);
    /// Vue "Modules" : résultat final d'une installation de module
    /// (payload : requestId, moduleId, success, message, error).
    void moduleInstallFinished(const QVariantMap& result);
    /// PROPOSITIONS-1 #4 — Live Lua REPL : une ligne envoyée via
    /// sendLuaReplLine a terminé (result contient "requestId", même
    /// convention que luaScriptExecutionFinished). Reçu par le frontend Qt
    /// (QWebChannel) ; PAS relayé par le pipe d'automatisation
    /// (getLuaReplLineResult poll à la place).
    void luaReplLineFinished(const QVariantMap& result);
    /// PROPOSITIONS-1 #2 — Pattern Learning.
    void patternLearningEngineDetected(const QString& gameName, const QVariantMap& engineInfo);
    void patternLearningClassified(const QString& address, const QVariantMap& classification);
    void patternLearningSuggestionReady(const QString& context, const QStringList& suggestions);
    void patternLearningTrackingUpdated(const QString& addressHex, const QVariantMap& analysis);
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
    /// Détecté automatiquement par WriteFreezeCoreManager au lieu d'attendre
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
    void watchSmartWriteIfPossible(uint64_t address, const QByteArray& writtenBytes, const QByteArray& originalBytes);
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
    ScanStateAccess scanState();
    ScanStateAccess scanState() const;
    AutoWriteStateAccess autoWriteState();

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

    bool                    m_attached{false};
    QString                 m_processName;
    int                     m_pid{0};
    killcore::ProcessHandle m_handle;
    std::unique_ptr<UiStringInvestigator> m_uiStringInvestigator;
    std::unique_ptr<MemoryHeatmapManager> m_memoryHeatmapManager;
    std::unique_ptr<MemoryTimelineManager> m_memoryTimelineManager;
    std::unique_ptr<PatternLearningManager> m_patternLearningManager;
    std::unique_ptr<ClrInspectorBridge> m_clrInspectorBridge;
    std::unique_ptr<killcore::WebView2Inspector> m_webView2Inspector;
    std::unique_ptr<CodePatchManager> m_codePatchManager;
    std::unique_ptr<ExternalToolProfiler> m_externalToolProfiler;
    std::unique_ptr<DebugFeatureManager> m_debugFeatureManager;
    std::unique_ptr<FreezeHotkeyOverlayManager> m_freezeHotkeyOverlayManager;
    std::unique_ptr<InvestigationNotebookManager> m_investigationNotebookManager;
    std::unique_ptr<KernelDriverManager> m_kernelDriverManager;
    std::unique_ptr<LuaReplManager> m_luaReplManager;
    std::unique_ptr<SaveFileInvestigator> m_saveFileInvestigator;
    std::unique_ptr<ScanningCoreManager> m_scanningCoreManager;
    std::unique_ptr<SettingsDiagnosticsManager> m_settingsDiagnosticsManager;
    std::unique_ptr<SmartSearchManager> m_smartSearchManager;
    std::unique_ptr<SmartWatchdogManager> m_smartWatchdogManager;
    std::unique_ptr<WriteFreezeCoreManager> m_writeFreezeCoreManager;
    std::unique_ptr<ProfileManager> m_profileManager;
    // Etat de blockProcessNetwork()/unblockProcessNetwork() : survit a un
    // detachProcess() pour que la regle pare-feu reste retirable meme apres
    // detach (voir doc au-dessus de la declaration Q_INVOKABLE).
    QString                 m_networkBlockRuleToken;
    QString                 m_networkBlockExePath;
    // Lag switch session (injection DLL + MinHook sur recv/WSARecv)
    std::unique_ptr<killcore::LagSwitchSession> m_lagSwitchSession;
    std::unique_ptr<killcore::HttpProxySession> m_httpProxySession;
    QVariantMap             m_webView2ActiveTarget;
    QString                 m_webView2Endpoint;
    int                     m_webView2BrowserProcessId{0};
    killcore::CandidateStore m_candidates;
    killcore::CandidateStore m_previousCandidates;
    QHash<uint64_t, QVariantList> m_candidateValueHistory;
    killcore::SnapshotStore  m_snapshot;
    QList<WriteRecord>       m_writeHistory;
    QList<AutoWriteTarget>   m_lastAutoWriteTargets;
    QList<AutoWriteTarget>   m_chatMemoryTargets;
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
    std::unique_ptr<AutomationPipeManager> m_automationPipeManager;
    std::shared_ptr<killcore::CancellationToken> m_activeScanCancellation;
    // Stealth mode state
    killcore::AntiDebugSession m_antiDebugSession;
    QString m_stealthProfile;
    bool m_stealthActive{false};
    bool m_stealthProcessMaskActive{false};
    bool m_stealthDllMaskActive{false};
    // Test automatique des champs candidats (voir testCandidateFieldsAsync) : etat
    // dedie, distinct de m_activeDebugCancellation qui est reserve aux operations
    // avec attach debugger (celle-ci ne fait que lire/ecrire de la memoire).
    bool                     m_candidateFieldTestInProgress{false};
    std::shared_ptr<killcore::CancellationToken> m_activeCandidateFieldTestCancellation;
    // Scripting Lua externe (executeLuaScriptAsync) : etat dedie, independant
    // des autres cancellations ci-dessus. Le token ne fait qu'armer le kill()
    // du QProcess lua.exe depuis le thread worker qui le possede -- pas de
    // manipulation cross-thread du QProcess lui-meme.
    bool                     m_luaScriptInProgress{false};
    std::shared_ptr<killcore::CancellationToken> m_activeLuaScriptCancellation;
    // Vue "Modules" (installation de module complémentaire) : état dédié de
    // l'installation en cours — un seul module à la fois, annulable via
    // cancelModuleInstall() (thread worker uniquement, pas le cas élevé UAC).
    bool                     m_moduleInstallInProgress{false};
    QString                  m_moduleInstallId;
    int                      m_moduleInstallRequestId{0};
    std::shared_ptr<killcore::CancellationToken> m_activeModuleInstallCancellation;
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
    // PHASE 119 -- pont callVueStoreAction() : page assignee par
    // setWebEnginePage() depuis main.cpp, jamais possedee ici.
    QPointer<QWebEnginePage> m_webEnginePage;

};

} // namespace killengine
