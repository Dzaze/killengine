#pragma once

#include <QString>
#include <QStringList>
#include <QVariantMap>

namespace killengine {

class ApplicationController;

class SmartSearchManager {
public:
    explicit SmartSearchManager(ApplicationController& controller);

    QVariantMap startSmartSearch(const QString& query);
    QVariantMap confirmChatMemoryWrite(const QString& value);
    QVariantMap confirmChatMemoryFreeze(const QString& value);
    QVariantMap confirmRewriteLastAutoWrite(const QString& value);
    QVariantMap startAutoResolve(const QString& query, const QVariantMap& options);
    QVariantMap getAutoResolveReport(int maxEvents) const;
    QVariantMap clearAutoResolveMemory(bool allProcesses);
    QVariantMap logAiAudit(const QString& event, const QVariantMap& payload);
    QVariantMap getRememberedPatterns() const;
    QVariantMap getWriteHistorySequence() const;
    QVariantMap replayWriteHistorySequence();
    QVariantMap clearWriteHistorySequence();
    QVariantMap getActiveChatMemoryTargets() const;
    QVariantMap clearActiveChatMemoryTargets();
    QVariantMap clearScanContext();
    void acknowledgePendingSmartSearchRecovery();
    QVariantMap getSmartSearchContext() const;

    QVariantMap activateChatMemoryTargetsFromQuery(const QString& query);
    QVariantMap writeChatMemoryTargetsFromQuery(const QString& query, const QString& value);
    QVariantMap freezeChatMemoryTargetsFromQuery(const QString& query, const QString& value);
    QVariantMap writeProfileTargetsFromQuery(const QString& query, const QString& value);
    QVariantMap buildFailureEscalationRecovery(const QString& query, const QStringList& numbers);
    void resetFailureEscalationState();

private:
    ApplicationController& m_controller;
};

} // namespace killengine