#pragma once

#include "process/process_handle.h"

#include <QObject>
#include <QString>
#include <QVariantMap>

#include <functional>
#include <memory>

namespace killcore {
class CancellationToken;
}

namespace killengine {

class SaveFileInvestigator : public QObject {
public:
    using NextRequestIdCallback = std::function<int()>;
    using ResultCallback = std::function<void(const QVariantMap&)>;

    SaveFileInvestigator(
        const killcore::ProcessHandle& handle,
        NextRequestIdCallback nextRequestId,
        ResultCallback saveFileWatchFinished,
        QObject* parent = nullptr);

    QVariantMap discoverProcessSaveFiles(int maxResults) const;
    QVariantMap compareSaveFileSnapshots(const QVariantList& before, const QVariantList& after) const;
    QVariantMap inspectProcessLocalSettings(int maxValues) const;
    QVariantMap readProcessSaveFileText(const QString& path, int maxBytes) const;
    QVariantMap patchProcessSaveFileBytes(const QString& path, const QString& findHex, const QString& replaceHex);
    QVariantMap watchSaveFileForChanges(const QString& path, const QVariantMap& options);
    QVariantMap startSaveFileWatchAsync(const QString& path, const QVariantMap& options);
    QVariantMap cancelSaveFileWatch();

private:
    const killcore::ProcessHandle& m_handle;
    NextRequestIdCallback m_nextRequestId;
    ResultCallback m_saveFileWatchFinished;
    bool m_saveFileWatchInProgress{false};
    std::shared_ptr<killcore::CancellationToken> m_activeSaveFileWatchCancellation;
};

} // namespace killengine
