#pragma once

#include <QString>
#include <QVariantMap>

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
    QString getLogFilePath() const;
    QString getSmartSearchDebugFilePath() const;
    QString getScanTelemetryFilePath() const;
    QVariantMap getSmartSearchDebugEvents(int maxEvents) const;
    QVariantMap clearSmartSearchDebugEvents();
    QVariantMap getLogTail(int maxLines) const;
    QVariantMap exportDiagnostics();
    QString smartSearchDebugFilePath() const;
    QString scanTelemetryFilePath() const;
    void appendSmartSearchDebug(const QString& event, const QVariantMap& payload) const;
    void appendScanTelemetry(const QString& event, const QVariantMap& payload) const;

private:
    ApplicationController& m_controller;
};

} // namespace killengine
