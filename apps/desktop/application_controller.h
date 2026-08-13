#pragma once

#include "ai_engine.h"
#include "candidates/candidate_store.h"
#include "freeze/freeze_manager.h"
#include "memory/memory_reader.h"
#include "process/process_handle.h"
#include "profiles/profile_store.h"
#include "snapshot/snapshot_store.h"

#include <QObject>
#include <QByteArray>
#include <QHash>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

#include <memory>

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

    /// Attache KillEngine à un processus.
    Q_INVOKABLE bool attachProcess(int pid);

    /// Détache le processus courant.
    Q_INVOKABLE void detachProcess();

    /// Retourne la carte mémoire du processus attaché.
    Q_INVOKABLE QVariantMap getMemoryMap() const;

    /// Lit un petit aperçu mémoire en hexadécimal depuis le processus attaché.
    Q_INVOKABLE QVariantMap readMemoryPreview(const QString& addressHex, int size) const;

    /// Cherche une valeur affichée sous forme de texte (ASCII / UTF-16LE) dans la mémoire.
    Q_INVOKABLE QVariantMap scanUiStrings(const QString& value, const QVariantMap& options) const;

    /// Relit des candidats texte et garde ceux qui correspondent à la nouvelle valeur affichée.
    Q_INVOKABLE QVariantMap trackUiStringCandidates(const QVariantList& candidates, const QString& value) const;

    /// Cherche des valeurs numériques plausibles autour d'une string UI déjà localisée.
    Q_INVOKABLE QVariantMap analyzeUiStringSources(
        const QVariantMap& stringCandidate,
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

    /// Restaure la dernière valeur écrasée par writeMemoryValue.
    Q_INVOKABLE QVariantMap rollbackLastWrite();

    /// Restaure toutes les écritures du dernier batch (auto-write).
    Q_INVOKABLE QVariantMap rollbackLastWriteBatch();

    /// Active/désactive un freeze simple sur une adresse.
    Q_INVOKABLE QVariantMap setFreezeValue(const QString& addressHex, const QString& valueType, const QString& value, bool enabled);

    // -----------------------------------------------------------------------
    // Phase 17 — Hardware Breakpoints / Find What Writes
    // -----------------------------------------------------------------------

    /// Trouve l'instruction qui écrit à une adresse (hardware breakpoint).
    /// C'est LA fonction qui permet de remonter à la vraie source gameplay
    /// quand on a trouvé une valeur "displayed" qui est réécrite par le jeu.
    /// options keys: size (1/2/4/8), timeoutMs (défaut 5000), maxHits (défaut 10)
    Q_INVOKABLE QVariantMap findWhatWrites(const QString& addressHex, const QVariantMap& options);

    /// Cherche une signature AOB dans les régions mémoire du processus.
    /// Pattern: "48 8B ?? ?? 89", options: executableOnly, imageOnly, startAddress, stopAddress, maxResults.
    Q_INVOKABLE QVariantMap scanAobPattern(const QString& pattern, const QVariantMap& options);

    /// Génère une signature AOB exacte à partir des octets autour d'une adresse d'instruction.
    /// options keys: beforeBytes, length.
    Q_INVOKABLE QVariantMap generateAobSignature(const QString& addressHex, const QVariantMap& options);

    /// Applique un patch de bytes exacts à une adresse code, en gardant les bytes originaux pour restauration.
    /// bytesText: "90 90 CC", options keys: verify.
    Q_INVOKABLE QVariantMap applyCodePatch(const QString& addressHex, const QString& bytesText, const QVariantMap& options);

    /// Restaure les bytes originaux d'un patch actif.
    Q_INVOKABLE QVariantMap restoreCodePatch(const QString& addressHex);

    /// Configure l'intervalle du freeze polling (10-2000 ms, 100 ms par défaut).
    Q_INVOKABLE QVariantMap setFreezeInterval(int intervalMs);

    /// Lance une recherche intelligente (Smart Search).
    /// Phase 0: stub qui logge la requête.
    Q_INVOKABLE QVariantMap startSmartSearch(const QString& query);

    /// Ping — permet au frontend de vérifier que le backend est connecté.
    Q_INVOKABLE QString ping(const QString& message);

    /// Retourne les paramètres persistants de l'application.
    Q_INVOKABLE QVariantMap getSettings() const;

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

    /// Liste tous les profils disponibles.
    Q_INVOKABLE QVariantList listProfiles();

    /// Charge un profil et retourne ses cibles.
    Q_INVOKABLE QVariantMap loadProfile(const QString& profileName);

    /// Supprime un profil.
    Q_INVOKABLE bool deleteProfile(const QString& profileName);

    /// Résout une cible de profil en adresse absolue pour le processus courant.
    Q_INVOKABLE QVariantMap resolveProfileTarget(const QString& profileName, const QString& targetName);

    /// Active une cible de profil pour l'utiliser directement depuis l'Assistant.
    Q_INVOKABLE QVariantMap activateProfileTarget(const QString& profileName, const QString& targetName);

    // -----------------------------------------------------------------------
    // Phase 14 — Pointer Chains (StarCraft 2 / jeux modernes)
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

signals:
    void attachmentChanged();
    void scanStarted();
    void scanProgress(int percent);
    void scanStatsUpdated(int candidateCount);
    void scanFinished(const QVariantMap& result);
    void aiMessage(const QString& message);
    void targetConfidenceChanged(int confidence);
    void targetFound(const QVariantMap& target);
    void processExited();
    void errorOccurred(const QString& message);

private:
    void applyFreezeTick();
    bool rememberCandidatesForUndo(QString* error = nullptr);
    void clearCandidateUndo();
    void clearCandidateValueHistory();
    void recordCandidateObservations(const QVariantList& observations);
    QVariantList candidateValueHistory(uint64_t address) const;
    void enrichSuggestedWritesWithHistory(QVariantList* suggestions) const;
    QVariantList filterAutoWriteSuggestionsByRegion(const QVariantList& suggestions, QVariantList* rejected) const;
    QVariantMap writeMemoryValueConfirmed(const QString& addressHex, const QString& valueType, const QString& value);
    QVariantMap rewriteLastAutoWriteTargets(const QString& value, const QString& query);
    QVariantMap activateChatMemoryTargetsFromQuery(const QString& query);
    QVariantMap writeChatMemoryTargetsFromQuery(const QString& query, const QString& value);
    QVariantMap freezeChatMemoryTargetsFromQuery(const QString& query, const QString& value);
    QVariantMap writeProfileTargetsFromQuery(const QString& query, const QString& value);
    QString smartSearchDebugFilePath() const;
    QString scanTelemetryFilePath() const;
    void appendSmartSearchDebug(const QString& event, const QVariantMap& payload) const;
    void appendScanTelemetry(const QString& event, const QVariantMap& payload) const;

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

    struct ActiveProfileTarget {
        QString profileName;
        QString targetName;
        QString groupName;
        uint64_t address{0};
        killcore::ValueType type{killcore::ValueType::Int32};
    };

    struct ActiveCodePatch {
        uint64_t address{0};
        QByteArray originalBytes;
        QByteArray patchBytes;
    };

    bool                    m_attached{false};
    QString                 m_processName;
    int                     m_pid{0};
    killcore::ProcessHandle m_handle;
    killcore::CandidateStore m_candidates;
    killcore::CandidateStore m_previousCandidates;
    QHash<uint64_t, QVariantList> m_candidateValueHistory;
    killcore::SnapshotStore  m_snapshot;
    killcore::FreezeManager  m_freeze;
    QTimer                   m_freezeTimer;
    uint64_t                 m_lastWriteAddress{0};
    QByteArray               m_lastWritePreviousValue;
    QList<WriteRecord>       m_writeHistory;
    QList<AutoWriteTarget>   m_lastAutoWriteTargets;
    QList<AutoWriteTarget>   m_chatMemoryTargets;
    QHash<uint64_t, ActiveCodePatch> m_activeCodePatches;
    qint64                   m_uiInvestigationStartedMs{0};
    QList<ActiveProfileTarget> m_activeProfileTargets;
    QStringList              m_autoWriteValueHistory;
    int                      m_lastBatchStartIndex{-1};
    int                      m_lastBatchEndIndex{-1};
    bool                     m_scanInProgress{false};
    bool                     m_hasPreviousCandidates{false};
    int                      m_nextScanRequestId{1};
    std::shared_ptr<killcore::CancellationToken> m_activeScanCancellation;
    killai::AIEngine         m_ai;
    bool                     m_smartSearchActive{false};
    QString                  m_smartSearchInitialValue;
    QString                  m_smartSearchTargetValue;
    QString                  m_smartSearchValueType{"Int32"};
};

} // namespace killengine
