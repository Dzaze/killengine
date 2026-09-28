#pragma once

#include "diagnostics/diagnostic_report_builder.h"

#include <QString>
#include <QVariantMap>

#include <optional>

namespace killengine {

class ApplicationController;

class SettingsDiagnosticsManager {
public:
    explicit SettingsDiagnosticsManager(ApplicationController& controller);

    QVariantMap getTemporaryStorageStatus() const;
    QVariantMap clearTemporaryStorage();
    QVariantMap getSettings() const;
    QVariantMap getAiModelStatus() const;
    QVariantMap browseForModelFile();
    QVariantMap saveSettings(const QVariantMap& settings);
    QVariantMap setUiLanguage(const QString& language);
    QString getLogFilePath() const;
    QString getSmartSearchDebugFilePath() const;
    QString getScanTelemetryFilePath() const;
    QVariantMap getSmartSearchDebugEvents(int maxEvents) const;
    QVariantMap clearSmartSearchDebugEvents();
    QVariantMap getLogTail(int maxLines) const;
    /// Ancien export brut sans argument -- conservé pour compatibilité pipe/TS
    /// (docs/PHASE_TRACKER.md #ux-produit-17, point 6). Passe désormais par le
    /// même assembleur borné/rédigé que prepareDiagnosticReport (options par
    /// défaut, récit vide) et écrit via QSaveFile avec vérification du nombre
    /// d'octets réellement écrits -- l'ancien code ne le faisait pas.
    QVariantMap exportDiagnostics();
    QString smartSearchDebugFilePath() const;
    QString scanTelemetryFilePath() const;
    void appendSmartSearchDebug(const QString& event, const QVariantMap& payload) const;
    void appendScanTelemetry(const QString& event, const QVariantMap& payload) const;

    // UX-PRODUIT-17 -- flux Préparer/Aperçu/Exporter. Une seule préparation
    // active à la fois (m_preparedReport), TTL 10 minutes vérifié à l'accès.
    // AUDIT-PIPE-A6 : `reportId` est optionnel (compatibilité des anciens
    // appelants sans argument, qui gardent l'ancien comportement -- servir
    // l'aperçu actuellement en cache quel qu'il soit). Un appelant qui le
    // fournit obtient la garantie qu'il lit/exporte bien l'aperçu qu'il a vu
    // au moment de préparer sa lecture, jamais un aperçu B qui l'aurait
    // silencieusement remplacé (`getPreparedDiagnosticReportPreview` renvoie
    // toujours le `reportId` courant, à retenir côté appelant pour ce
    // paramètre).
    QVariantMap prepareDiagnosticReport(const QVariantMap& options);
    QVariantMap getPreparedDiagnosticReportPreview(const QString& reportId = QString()) const;
    QVariantMap getPreparedDiagnosticReportSection(
        const QString& sectionId, qint64 offset, qint64 limit, const QString& reportId = QString()) const;
    QVariantMap exportPreparedDiagnosticReport(const QString& reportId = QString());

private:
    ApplicationController& m_controller;
    std::optional<killcore::PreparedDiagnosticReport> m_preparedReport;

    bool isPreparedReportValid() const;
    /// AUDIT-PIPE-A6 : `reportId` vide = pas de vérification (compatibilité).
    /// Sinon, refuse explicitement (sans toucher à l'état) si l'aperçu en
    /// cache n'existe plus, a expiré, ou a été remplacé par une préparation
    /// plus récente -- jamais servir/exporter silencieusement un autre
    /// rapport que celui demandé.
    bool checkPreparedReportAccess(const QString& reportId, QVariantMap* errorResult) const;
    /// AUDIT-PIPE-A5 (réutilisée telle quelle) : construit un rapport
    /// INDÉPENDANT à partir de `options`, sans jamais toucher
    /// `m_preparedReport` -- appelée à la fois par prepareDiagnosticReport()
    /// (qui stocke le résultat) et par exportDiagnostics() legacy (qui ne le
    /// stocke jamais, pour ne pas remplacer un aperçu en cours de
    /// consultation -- AUDIT-PIPE-A6).
    killcore::BuildResult buildFreshDiagnosticReport(const QVariantMap& options) const;
    QByteArray buildExportPayload(const killcore::PreparedDiagnosticReport& report) const;
    QVariantMap writeReportToPath(const killcore::PreparedDiagnosticReport& report, const QString& path) const;
};

} // namespace killengine
