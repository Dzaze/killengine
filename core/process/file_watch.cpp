#include "process/file_watch.h"
#include "localization/localization.h"
#include "memory/memory_reader.h"

#ifdef Q_OS_WIN
#include <windows.h>
#endif

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>

#include <algorithm>
#include <vector>

namespace killcore {

#ifdef Q_OS_WIN
namespace {

QString actionToChangeType(DWORD action) {
    switch (action) {
        case FILE_ACTION_MODIFIED: return "modified";
        case FILE_ACTION_ADDED: return "added";
        case FILE_ACTION_REMOVED: return "removed";
        case FILE_ACTION_RENAMED_OLD_NAME:
        case FILE_ACTION_RENAMED_NEW_NAME:
            return "renamed";
        default: return "changed";
    }
}

} // namespace
#endif

bool watchFileForChanges(
    const QString& path,
    int timeoutMs,
    CancellationToken* cancellation,
    FileWatchOutcome* outcome,
    QString* error) {

    if (error) error->clear();
    if (!outcome) {
        return false;
    }
    *outcome = FileWatchOutcome{};

#ifdef Q_OS_WIN
    const QFileInfo info(path);
    const QString dirPath = QDir::toNativeSeparators(info.absolutePath());
    const QString targetName = info.fileName();
    if (dirPath.isEmpty() || targetName.isEmpty()) {
        if (error) *error = KE_TXT("Chemin invalide.", "Invalid path.");
        return false;
    }

    HANDLE hDir = CreateFileW(
        reinterpret_cast<LPCWSTR>(dirPath.utf16()),
        FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
        nullptr);
    if (hDir == INVALID_HANDLE_VALUE) {
        if (error) *error = KE_TXT("Ouverture du dossier parent impossible (code %1).", "Unable to open the parent folder (code %1).").arg(GetLastError());
        return false;
    }

    HANDLE hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!hEvent) {
        CloseHandle(hDir);
        if (error) *error = KE_TXT("Création de l'event de surveillance impossible.", "Unable to create the monitoring event.");
        return false;
    }

    const qint64 clampedTimeout = timeoutMs > 0 ? timeoutMs : 5000;
    const DWORD pollMs = 200;
    // Buffer aligné DWORD (exigence de FILE_NOTIFY_INFORMATION) -- un
    // std::vector<DWORD> garantit l'alignement sans jongler avec alignas.
    std::vector<DWORD> alignedBuffer(2048); // 8192 octets
    BYTE* buffer = reinterpret_cast<BYTE*>(alignedBuffer.data());
    const DWORD bufferSize = static_cast<DWORD>(alignedBuffer.size() * sizeof(DWORD));

    QElapsedTimer elapsed;
    elapsed.start();

    bool changed = false;
    bool cancelled = false;
    QString changeType;

    while (elapsed.elapsed() < clampedTimeout) {
        if (cancellation && cancellation->isCancelled()) {
            cancelled = true;
            break;
        }

        OVERLAPPED overlapped{};
        overlapped.hEvent = hEvent;
        ResetEvent(hEvent);

        DWORD unusedBytes = 0;
        const BOOL issued = ReadDirectoryChangesW(
            hDir,
            buffer,
            bufferSize,
            FALSE, // un seul fichier ciblé, pas besoin de descendre dans les sous-dossiers
            FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE
                | FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_CREATION,
            &unusedBytes,
            &overlapped,
            nullptr);
        if (!issued) {
            if (error) *error = KE_TXT("ReadDirectoryChangesW a échoué (code %1).", "ReadDirectoryChangesW failed (code %1).").arg(GetLastError());
            CloseHandle(hEvent);
            CloseHandle(hDir);
            return false;
        }

        const qint64 remaining = std::max<qint64>(0, clampedTimeout - elapsed.elapsed());
        const DWORD waitMs = static_cast<DWORD>(std::min<qint64>(pollMs, remaining));
        const DWORD waitResult = WaitForSingleObject(hEvent, waitMs);

        if (waitResult == WAIT_OBJECT_0) {
            DWORD bytesTransferred = 0;
            if (!GetOverlappedResult(hDir, &overlapped, &bytesTransferred, FALSE) || bytesTransferred == 0) {
                continue; // notification vide/inexploitable, on reboucle
            }

            const BYTE* cursor = buffer;
            for (;;) {
                const auto* entry = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(cursor);
                const QString notifiedName = QString::fromWCharArray(
                    entry->FileName, static_cast<int>(entry->FileNameLength / sizeof(WCHAR)));
                if (notifiedName.compare(targetName, Qt::CaseInsensitive) == 0) {
                    changed = true;
                    changeType = actionToChangeType(entry->Action);
                }
                if (entry->NextEntryOffset == 0) {
                    break;
                }
                cursor += entry->NextEntryOffset;
            }

            if (changed) {
                break;
            }
        } else if (waitResult == WAIT_TIMEOUT) {
            // Annule la requête en attente avant de reboucler, sinon les appels
            // ReadDirectoryChangesW successifs s'accumulent sur le handle.
            DWORD cancelBytes = 0;
            CancelIoEx(hDir, &overlapped);
            GetOverlappedResult(hDir, &overlapped, &cancelBytes, TRUE);
            continue;
        } else {
            if (error) *error = KE_TXT("Attente de notification échouée (code %1).", "Waiting for notification failed (code %1).").arg(GetLastError());
            DWORD cancelBytes = 0;
            CancelIoEx(hDir, &overlapped);
            GetOverlappedResult(hDir, &overlapped, &cancelBytes, TRUE);
            CloseHandle(hEvent);
            CloseHandle(hDir);
            return false;
        }
    }

    CloseHandle(hEvent);
    CloseHandle(hDir);

    outcome->changed = changed;
    outcome->cancelled = cancelled;
    outcome->changeType = changeType;
    return true;
#else
    (void)path;
    (void)timeoutMs;
    (void)cancellation;
    if (error) *error = KE_TXT("Non supporté sur cette plateforme.", "Not supported on this platform.");
    return false;
#endif
}

} // namespace killcore
