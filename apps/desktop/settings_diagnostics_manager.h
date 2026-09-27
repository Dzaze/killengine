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
    QVariantMap prepareDiagnosticReport(const QVariantMap& options);
    QVariantMap getPreparedDiagnosticReportPreview() const;
    QVariantMap getPreparedDiagnosticReportSection(const QString& sectionId, qint64 offset, qint64 limit) const;
    QVariantMap exportPreparedDiagnosticReport();

private:
    ApplicationController& m_controller;
    std::optional<killcore::PreparedDiagnosticReport> m_preparedReport;

    bool isPreparedReportValid() const;
    QByteArray buildExportPayload(const killcore::PreparedDiagnosticReport& report) const;
    QVariantMap writeReportToPath(const killcore::PreparedDiagnosticReport& report, const QString& path) const;
};

} // namespace killengine
