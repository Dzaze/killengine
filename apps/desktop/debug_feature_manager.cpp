#include "debug_feature_manager.h"

#include "debug/hardware_breakpoint.h"
#include "debug/inprocess_breakpoint.h"
#include "debug/page_guard.h"
#include "debug/speedhack.h"
#include "inject/api_hook.h"
#include "localization/localization.h"
#include "logging/logger.h"
#include "memory/memory_reader.h"
#include "process/process_handle.h"
#include "scanner/scan_types.h"
#include "patch/instruction_patch_suggester.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QPointer>
#include <QStringList>
#include <QThread>
#include <QVariantList>

#include <algorithm>
#include <thread>

namespace killengine {
namespace {
QVariantMap breakpointHitToVariant(const killcore::BreakpointHit& hit) {
    QVariantMap item;
    item["address"] = QString::number(hit.address, 16).toUpper();
    item["instructionPointer"] = QString::number(hit.instructionPointer, 16).toUpper();
    item["threadId"] = static_cast<qulonglong>(hit.threadId);
    item["valueBefore"] = static_cast<qulonglong>(hit.valueBefore);
    item["valueAfter"] = static_cast<qulonglong>(hit.valueAfter);
    item["module"] = hit.module;
    item["moduleOffset"] = QString::number(hit.moduleOffset, 16).toUpper();
    item["rax"] = QString::number(hit.rax, 16).toUpper();
    item["rbx"] = QString::number(hit.rbx, 16).toUpper();
    item["rcx"] = QString::number(hit.rcx, 16).toUpper();
    item["rdx"] = QString::number(hit.rdx, 16).toUpper();
    item["rsi"] = QString::number(hit.rsi, 16).toUpper();
    item["rdi"] = QString::number(hit.rdi, 16).toUpper();
    item["rbp"] = QString::number(hit.rbp, 16).toUpper();
    item["rsp"] = QString::number(hit.rsp, 16).toUpper();
    item["r8"] = QString::number(hit.r8, 16).toUpper();
    item["r9"] = QString::number(hit.r9, 16).toUpper();
    item["r10"] = QString::number(hit.r10, 16).toUpper();
    item["r11"] = QString::number(hit.r11, 16).toUpper();
    item["r12"] = QString::number(hit.r12, 16).toUpper();
    item["r13"] = QString::number(hit.r13, 16).toUpper();
    item["r14"] = QString::number(hit.r14, 16).toUpper();
    item["r15"] = QString::number(hit.r15, 16).toUpper();
    item["xmm0Hex"] = QString::fromLatin1(hit.xmm0.toHex(' ').toUpper());
    return item;
}

QVariantMap inProcessBreakpointHitToVariant(const killcore::InProcessBreakpointHit& hit) {
    QVariantMap item;
    item["instructionPointer"] = QString::number(hit.instructionPointer, 16).toUpper();
    item["threadId"] = static_cast<qulonglong>(hit.threadId);
    item["module"] = hit.module;
    item["moduleOffset"] = QString::number(hit.moduleOffset, 16).toUpper();
    item["rax"] = QString::number(hit.rax, 16).toUpper();
    item["rcx"] = QString::number(hit.rcx, 16).toUpper();
    item["rdx"] = QString::number(hit.rdx, 16).toUpper();
    item["rbp"] = QString::number(hit.rbp, 16).toUpper();
    item["rsp"] = QString::number(hit.rsp, 16).toUpper();
    item["r8"] = QString::number(hit.r8, 16).toUpper();
    item["r9"] = QString::number(hit.r9, 16).toUpper();
    item["xmm0Hex"] = QString::fromLatin1(hit.xmm0.toHex(' ').toUpper());
    return item;
}

QString codeReadProtectionHint(uint32_t errorCode) {
    if (errorCode == 299) {
        return KE_TXT(
            "Le code de ce module semble protégé contre la lecture externe "
            "(fréquent sur les exécutables Microsoft Store/UWP signés). "
            "Génération de signature/patch impossible sur cette instruction — "
            "essaie Freeze ou une écriture groupée sur la donnée plutôt qu'un "
            "patch du code.",
            "This module's code appears protected against external reads "
            "(common on signed Microsoft Store/UWP executables). "
            "Cannot generate a signature/patch for this instruction — "
            "try Freeze or a batched write on the data instead of a "
            "code patch.");
    }
    return QString();
}

bool parseHexAddress(const QString& addressHex, uint64_t* address) {
    if (!address) {
        return false;
    }

    QString normalized = addressHex.trimmed();
    if (normalized.startsWith("0x", Qt::CaseInsensitive)) {
        normalized = normalized.mid(2);
    }

    bool ok = false;
    const uint64_t parsed = normalized.toULongLong(&ok, 16);
    if (!ok || parsed == 0) {
        return false;
    }

    *address = parsed;
    return true;
}

QString resolvePageGuardHandlerPath() {
    const QDir appDir(QCoreApplication::applicationDirPath());
    const QStringList candidates = {
        appDir.filePath("KillEnginePageGuardHandler.dll"),
        appDir.filePath("../lib/KillEnginePageGuardHandler.dll"),
    };
    for (const auto& candidate : candidates) {
        if (QFileInfo::exists(candidate)) {
            return QFileInfo(candidate).absoluteFilePath();
        }
    }
    return QString();
}

// Meme demarche que resolvePageGuardHandlerPath() pour
// KillEngineInProcessBreakpointHandler.dll (core/CMakeLists.txt).
QString resolveInProcessBreakpointHandlerPath() {
    const QDir appDir(QCoreApplication::applicationDirPath());
    const QStringList candidates = {
        appDir.filePath("KillEngineInProcessBreakpointHandler.dll"),
        appDir.filePath("../lib/KillEngineInProcessBreakpointHandler.dll"),
    };
    for (const auto& candidate : candidates) {
        if (QFileInfo::exists(candidate)) {
            return QFileInfo(candidate).absoluteFilePath();
        }
    }
    return QString();
}

// Meme demarche que resolvePageGuardHandlerPath() pour
// KillEngineSpeedhackHandler.dll (core/CMakeLists.txt).
QString resolveApiHookHandlerPath() {
    // KillEngineApiHookHandler.dll est produite par core/CMakeLists.txt (meme
    // repertoire de sortie que les autres handlers injectes).
    const QDir appDir(QCoreApplication::applicationDirPath());
    const QStringList candidates = {
        appDir.filePath("KillEngineApiHookHandler.dll"),
        appDir.filePath("../lib/KillEngineApiHookHandler.dll"),
    };
    for (const QString& candidate : candidates) {
        if (QFile::exists(candidate)) {
            return candidate;
        }
    }
    return QString();
}

QString resolveSpeedhackHandlerPath() {
    const QDir appDir(QCoreApplication::applicationDirPath());
    const QStringList candidates = {
        appDir.filePath("KillEngineSpeedhackHandler.dll"),
        appDir.filePath("../lib/KillEngineSpeedhackHandler.dll"),
    };
    for (const auto& candidate : candidates) {
        if (QFileInfo::exists(candidate)) {
            return QFileInfo(candidate).absoluteFilePath();
        }
    }
    return QString();
}
} // namespace

DebugFeatureManager::DebugFeatureManager(
    const killcore::ProcessHandle& handle,
    killcore::FreezeManager& freeze,
    TelemetryCallback telemetry,
    IsAttachedCallback isAttached,
    PidCallback pid,
    NextRequestIdCallback nextRequestId,
    ResultCallback findWhatWritesFinished,
    ResultCallback findWhatAccessesFinished,
    ResultCallback pageGuardWatchFinished,
    ResultCallback inProcessBreakpointWatchFinished,
    QObject* parent)
    : QObject(parent)
    , m_handle(handle)
    , m_freeze(freeze)
    , m_appendScanTelemetry(std::move(telemetry))
    , m_isAttached(std::move(isAttached))
    , m_pid(std::move(pid))
    , m_nextRequestId(std::move(nextRequestId))
    , m_findWhatWritesFinished(std::move(findWhatWritesFinished))
    , m_findWhatAccessesFinished(std::move(findWhatAccessesFinished))
    , m_pageGuardWatchFinished(std::move(pageGuardWatchFinished))
    , m_inProcessBreakpointWatchFinished(std::move(inProcessBreakpointWatchFinished)) {}

DebugFeatureManager::~DebugFeatureManager() {
    stopBreakpointFreeze();
    stopInjectionSessions();
}

void DebugFeatureManager::stopBreakpointFreeze() {
    if (m_breakpointFreeze) {
        m_breakpointFreeze->stop();
    }
}

void DebugFeatureManager::stopInjectionSessions() {
    if (m_speedhackSession) {
        m_speedhackSession->stop();
        m_speedhackSession.reset();
    }
    if (m_apiHookSession) {
        m_apiHookSession->stop();
        m_apiHookSession.reset();
    }
}

void DebugFeatureManager::resetHardwareBreakpointStateForPreviousTarget(int previousPid) {
    if (previousPid <= 0) {
        return; // rien n'etait attache avant
    }
    const uint32_t pid = static_cast<uint32_t>(previousPid);

    const killcore::ProcessHandle probe(pid, killcore::ProcessAccess::ReadOnly);
    KE_LOG_INFO() << "resetHardwareBreakpointStateForPreviousTarget: previousPid=" << pid
                  << (probe.isValid()
                          ? " (toujours vivante, detachement volontaire)"
                          : " (n'existe plus -- cible morte/redemarree, nettoyage avant rattachement)");

    if (m_activeInProcessBreakpointSession) {
        m_activeInProcessBreakpointSession->stop();
    }
    if (m_inProcessBreakpointFreezeSession) {
        m_inProcessBreakpointFreezeSession->stop();
    }
    m_inProcessBreakpointWatchInProgress = false;

    killcore::HwBreakpointArbiter::instance().resetForPid(pid);
}

bool DebugFeatureManager::deferDetachIfBusy() {
    if (m_findWhatWritesInProgress) {
        if (m_activeDebugCancellation) {
            m_activeDebugCancellation->cancel();
        }
        KE_LOG_INFO() << "Detach deferred because Find What Writes is still running.";
        return true;
    }
    if (m_pageGuardWatchInProgress) {
        if (m_activePageGuardSession) {
            m_activePageGuardSession->stop();
        }
        KE_LOG_INFO() << "Detach deferred because Page Guard watch is still running.";
        return true;
    }
    return false;
}
QVariantMap DebugFeatureManager::findWhatWrites(const QString& addressHex, const QVariantMap& options) {
    QVariantMap result;
    result["success"] = false;
    result["address"] = addressHex;

    if (!m_isAttached() || m_pid() <= 0) {
        result["error"] = KE_TXT("Aucun processus attaché.", "No process attached.");
        return result;
    }

    uint64_t address = 0;
    if (!parseHexAddress(addressHex, &address)) {
        result["error"] = KE_TXT("Adresse invalide.", "Invalid address.");
        return result;
    }

    const int sizeBytes = std::clamp(options.value("size", 4).toInt(), 1, 8);
    killcore::BreakpointSize breakpointSize = killcore::BreakpointSize::DWord;
    if (sizeBytes <= 1) {
        breakpointSize = killcore::BreakpointSize::Byte;
    } else if (sizeBytes <= 2) {
        breakpointSize = killcore::BreakpointSize::Word;
    } else if (sizeBytes <= 4) {
        breakpointSize = killcore::BreakpointSize::DWord;
    } else {
        breakpointSize = killcore::BreakpointSize::QWord;
    }

    const int timeoutMs = std::clamp(options.value("timeoutMs", 5000).toInt(), 250, 15000);
    const int maxHitsInt = std::clamp(options.value("maxHits", 10).toInt(), 1, 100);

    KE_LOG_INFO() << "findWhatWrites(address=0x" << std::hex << address
                  << ", pid=" << std::dec << m_pid()
                  << ", size=" << sizeBytes
                  << ", timeoutMs=" << timeoutMs
                  << ", maxHits=" << maxHitsInt << ")";

    const auto hits = killcore::findWhatWrites(
        static_cast<uint32_t>(m_pid()),
        address,
        breakpointSize,
        timeoutMs,
        static_cast<size_t>(maxHitsInt));

    QVariantList hitList;
    for (const auto& hit : hits) {
        QVariantMap item;
        item["address"] = QString::number(hit.address, 16).toUpper();
        item["instructionPointer"] = QString::number(hit.instructionPointer, 16).toUpper();
        item["threadId"] = static_cast<qulonglong>(hit.threadId);
        item["valueBefore"] = static_cast<qulonglong>(hit.valueBefore);
        item["valueAfter"] = static_cast<qulonglong>(hit.valueAfter);
        item["module"] = hit.module;
        item["moduleOffset"] = QString::number(hit.moduleOffset, 16).toUpper();
        hitList.append(item);
    }

    result["success"] = true;
    result["hits"] = hitList;
    result["hitCount"] = hitList.size();
    result["size"] = sizeBytes;
    result["timeoutMs"] = timeoutMs;
    result["maxHits"] = maxHitsInt;
    result["warning"] = KE_TXT("Cette fonction attache KillEngine comme debugger au processus cible pendant la capture.", "This function attaches KillEngine as a debugger to the target process during the capture.");
    result["error"] = hits.isEmpty()
        ? KE_TXT("Aucune écriture capturée pendant la fenêtre d'observation.", "No write captured during the observation window.")
        : QString();
    return result;
}

QVariantMap DebugFeatureManager::findWhatAccesses(const QString& addressHex, const QVariantMap& options) {
    QVariantMap result;
    result["success"] = false;
    result["address"] = addressHex;

    if (!m_isAttached() || m_pid() <= 0) {
        result["error"] = KE_TXT("Aucun processus attaché.", "No process attached.");
        return result;
    }

    uint64_t address = 0;
    if (!parseHexAddress(addressHex, &address)) {
        result["error"] = KE_TXT("Adresse invalide.", "Invalid address.");
        return result;
    }

    const int sizeBytes = std::clamp(options.value("size", 4).toInt(), 1, 8);
    killcore::BreakpointSize breakpointSize = killcore::BreakpointSize::DWord;
    if (sizeBytes <= 1) {
        breakpointSize = killcore::BreakpointSize::Byte;
    } else if (sizeBytes <= 2) {
        breakpointSize = killcore::BreakpointSize::Word;
    } else if (sizeBytes <= 4) {
        breakpointSize = killcore::BreakpointSize::DWord;
    } else {
        breakpointSize = killcore::BreakpointSize::QWord;
    }

    const int timeoutMs = std::clamp(options.value("timeoutMs", 5000).toInt(), 250, 15000);
    const int maxHitsInt = std::clamp(options.value("maxHits", 10).toInt(), 1, 100);

    KE_LOG_INFO() << "findWhatAccesses(address=0x" << std::hex << address
                  << ", pid=" << std::dec << m_pid()
                  << ", size=" << sizeBytes
                  << ", timeoutMs=" << timeoutMs
                  << ", maxHits=" << maxHitsInt << ")";

    const auto hits = killcore::findWhatAccesses(
        static_cast<uint32_t>(m_pid()),
        address,
        breakpointSize,
        timeoutMs,
        static_cast<size_t>(maxHitsInt));

    QVariantList hitList;
    for (const auto& hit : hits) {
        QVariantMap item;
        item["address"] = QString::number(hit.address, 16).toUpper();
        item["instructionPointer"] = QString::number(hit.instructionPointer, 16).toUpper();
        item["threadId"] = static_cast<qulonglong>(hit.threadId);
        item["valueBefore"] = static_cast<qulonglong>(hit.valueBefore);
        item["valueAfter"] = static_cast<qulonglong>(hit.valueAfter);
        item["module"] = hit.module;
        item["moduleOffset"] = QString::number(hit.moduleOffset, 16).toUpper();
        hitList.append(item);
    }

    result["success"] = true;
    result["hits"] = hitList;
    result["hitCount"] = hitList.size();
    result["size"] = sizeBytes;
    result["timeoutMs"] = timeoutMs;
    result["maxHits"] = maxHitsInt;
    result["warning"] = KE_TXT(
        "Cette fonction attache KillEngine comme debugger au processus cible pendant la capture. "
        "Capture lecture ET écriture (contrairement à findWhatWrites) : peut révéler une "
        "instruction de vérification/comparaison distincte de celle qui écrit.",
        "This function attaches KillEngine as a debugger to the target process during the capture. "
        "Captures reads AND writes (unlike findWhatWrites): may reveal a check/comparison "
        "instruction distinct from the one that writes.");
    result["error"] = hits.isEmpty()
        ? KE_TXT("Aucun accès capturé pendant la fenêtre d'observation.", "No access captured during the observation window.")
        : QString();
    return result;
}

QVariantMap DebugFeatureManager::findWhatExecutes(const QString& instructionAddressHex, const QVariantMap& options) {
    QVariantMap result;
    result["success"] = false;
    result["address"] = instructionAddressHex;

    if (!m_isAttached() || m_pid() <= 0) {
        result["error"] = KE_TXT("Aucun processus attaché.", "No process attached.");
        return result;
    }

    uint64_t address = 0;
    if (!parseHexAddress(instructionAddressHex, &address)) {
        result["error"] = KE_TXT("Adresse d'instruction invalide.", "Invalid instruction address.");
        return result;
    }

    const int timeoutMs = std::clamp(options.value("timeoutMs", 5000).toInt(), 250, 15000);
    const int maxHitsInt = std::clamp(options.value("maxHits", 10).toInt(), 1, 100);

    KE_LOG_INFO() << "findWhatExecutes(address=0x" << std::hex << address
                  << ", pid=" << std::dec << m_pid()
                  << ", timeoutMs=" << timeoutMs
                  << ", maxHits=" << maxHitsInt << ")";

    const auto hits = killcore::findWhatExecutes(
        static_cast<uint32_t>(m_pid()),
        address,
        timeoutMs,
        static_cast<size_t>(maxHitsInt));

    QVariantList hitList;
    for (const auto& hit : hits) {
        hitList.append(breakpointHitToVariant(hit));
    }

    result["success"] = true;
    result["hits"] = hitList;
    result["hitCount"] = hitList.size();
    result["timeoutMs"] = timeoutMs;
    result["maxHits"] = maxHitsInt;
    result["warning"] = KE_TXT(
        "Cette fonction attache KillEngine comme debugger au processus cible pendant la capture. "
        "Break on execute : aucune ecriture ni patch, les hits exposent les registres runtime.",
        "This function attaches KillEngine as a debugger to the target process during the capture. "
        "Break on execute: no write or patch, hits expose the runtime registers.");
    result["error"] = hits.isEmpty()
        ? KE_TXT("Aucune exécution capturée pendant la fenêtre d'observation.", "No execution captured during the observation window.")
        : QString();
    m_appendScanTelemetry("find_what_executes", {
        {"success", true},
        {"address", instructionAddressHex},
        {"hitCount", hitList.size()},
        {"timeoutMs", timeoutMs},
        {"maxHits", maxHitsInt},
    });
    return result;
}

QVariantMap DebugFeatureManager::findWhatWritesAsync(const QString& addressHex, const QVariantMap& options) {
    QVariantMap result;
    result["success"] = false;
    result["started"] = false;
    result["address"] = addressHex;

    if (m_findWhatWritesInProgress) {
        result["error"] = KE_TXT("Une capture Find What Writes est déjà en cours.", "A Find What Writes capture is already in progress.");
        return result;
    }
    if (!m_isAttached() || m_pid() <= 0) {
        result["error"] = KE_TXT("Aucun processus attaché.", "No process attached.");
        return result;
    }

    uint64_t address = 0;
    if (!parseHexAddress(addressHex, &address)) {
        result["error"] = KE_TXT("Adresse invalide.", "Invalid address.");
        return result;
    }

    const int sizeBytes = std::clamp(options.value("size", 4).toInt(), 1, 8);
    killcore::BreakpointSize breakpointSize = killcore::BreakpointSize::DWord;
    if (sizeBytes <= 1) {
        breakpointSize = killcore::BreakpointSize::Byte;
    } else if (sizeBytes <= 2) {
        breakpointSize = killcore::BreakpointSize::Word;
    } else if (sizeBytes <= 4) {
        breakpointSize = killcore::BreakpointSize::DWord;
    } else {
        breakpointSize = killcore::BreakpointSize::QWord;
    }

    const int timeoutMs = std::clamp(options.value("timeoutMs", 5000).toInt(), 250, 15000);
    const int maxHitsInt = std::clamp(options.value("maxHits", 10).toInt(), 1, 100);
    const int requestId = m_nextRequestId();
    const int pid = m_pid();
    const QString requestedAddress = addressHex;
    const QPointer<DebugFeatureManager> self(this);
    auto cancellation = std::make_shared<killcore::CancellationToken>();

    m_findWhatWritesInProgress = true;
    m_activeDebugCancellation = cancellation;

    KE_LOG_INFO() << "findWhatWritesAsync(address=0x" << std::hex << address
                  << ", pid=" << std::dec << pid
                  << ", size=" << sizeBytes
                  << ", timeoutMs=" << timeoutMs
                  << ", maxHits=" << maxHitsInt
                  << ", requestId=" << requestId << ")";

    std::thread([self, requestId, pid, address, requestedAddress, breakpointSize, sizeBytes, timeoutMs, maxHitsInt, cancellation]() {
        const auto hits = killcore::findWhatWrites(
            static_cast<uint32_t>(pid),
            address,
            breakpointSize,
            timeoutMs,
            static_cast<size_t>(maxHitsInt),
            cancellation.get());
        const bool cancelled = cancellation->isCancelled();

        if (!self) {
            return;
        }

        QMetaObject::invokeMethod(self.data(), [self, requestId, requestedAddress, sizeBytes, timeoutMs, maxHitsInt, hits, cancelled]() {
            if (!self) {
                return;
            }

            QVariantList hitList;
            for (const auto& hit : hits) {
                QVariantMap item;
                item["address"] = QString::number(hit.address, 16).toUpper();
                item["instructionPointer"] = QString::number(hit.instructionPointer, 16).toUpper();
                item["threadId"] = static_cast<qulonglong>(hit.threadId);
                item["valueBefore"] = static_cast<qulonglong>(hit.valueBefore);
                item["valueAfter"] = static_cast<qulonglong>(hit.valueAfter);
                item["module"] = hit.module;
                item["moduleOffset"] = QString::number(hit.moduleOffset, 16).toUpper();
                hitList.append(item);
            }

            QVariantMap finished;
            finished["requestId"] = requestId;
            finished["kind"] = "find_what_writes";
            finished["success"] = true;
            finished["address"] = requestedAddress;
            finished["hits"] = hitList;
            finished["hitCount"] = hitList.size();
            finished["size"] = sizeBytes;
            finished["timeoutMs"] = timeoutMs;
            finished["maxHits"] = maxHitsInt;
            finished["cancelled"] = cancelled;
            finished["warning"] = KE_TXT("Cette fonction attache KillEngine comme debugger au processus cible pendant la capture.", "This function attaches KillEngine as a debugger to the target process during the capture.");
            finished["error"] = cancelled
                ? KE_TXT("Capture Find What Writes annulée.", "Find What Writes capture cancelled.")
                : hits.isEmpty()
                ? KE_TXT("Aucune écriture capturée pendant la fenêtre d'observation.", "No write captured during the observation window.")
                : QString();

            self->m_findWhatWritesInProgress = false;
            self->m_activeDebugCancellation.reset();
            self->m_findWhatWritesFinished(finished);
        }, Qt::QueuedConnection);
    }).detach();

    result["success"] = true;
    result["started"] = true;
    result["requestId"] = requestId;
    result["size"] = sizeBytes;
    result["timeoutMs"] = timeoutMs;
    result["maxHits"] = maxHitsInt;
    result["error"] = "";
    return result;
}

QVariantMap DebugFeatureManager::cancelFindWhatWrites() {
    QVariantMap result;
    result["success"] = false;
    if (!m_findWhatWritesInProgress || !m_activeDebugCancellation) {
        result["error"] = KE_TXT("Aucune capture Find What Writes active à annuler.", "No active Find What Writes capture to cancel.");
        return result;
    }

    m_activeDebugCancellation->cancel();
    result["success"] = true;
    result["error"] = "";
    return result;
}

QVariantMap DebugFeatureManager::startPageGuardWatchAsync(const QString& addressHex, const QVariantMap& options) {
    QVariantMap result;
    result["success"] = false;
    result["started"] = false;
    result["address"] = addressHex;

    if (m_pageGuardWatchInProgress) {
        result["error"] = KE_TXT("Une capture Page Guard est déjà en cours.", "A Page Guard capture is already in progress.");
        return result;
    }
    if (!m_isAttached() || m_pid() <= 0 || !m_handle.isValid()) {
        result["error"] = KE_TXT("Aucun processus attaché.", "No process attached.");
        return result;
    }

    uint64_t address = 0;
    if (!parseHexAddress(addressHex, &address)) {
        result["error"] = KE_TXT("Adresse invalide.", "Invalid address.");
        return result;
    }

    const QString handlerPath = resolvePageGuardHandlerPath();
    if (handlerPath.isEmpty()) {
        result["error"] = KE_TXT("KillEnginePageGuardHandler.dll introuvable à côté de KillEngine.exe.", "KillEnginePageGuardHandler.dll not found next to KillEngine.exe.");
        return result;
    }

    killcore::PageGuardConfig config;
    config.address = address;
    config.size = static_cast<size_t>(std::clamp(options.value("size", 4).toInt(), 1, 4096));
    config.captureWrites = options.value("captureWrites", true).toBool();
    config.captureReads = options.value("captureReads", false).toBool();
    config.timeoutMs = std::clamp(options.value("timeoutMs", 5000).toInt(), 250, 15000);
    config.maxHits = static_cast<size_t>(std::clamp(options.value("maxHits", 10).toInt(), 1, 100));
    config.injectedHandlerPath = handlerPath;

    const int requestId = m_nextRequestId();
    const QString requestedAddress = addressHex;
    const QPointer<DebugFeatureManager> self(this);
    auto session = std::make_shared<killcore::PageGuardSession>();
    const uint32_t pid = m_pid();

    m_pageGuardWatchInProgress = true;
    m_activePageGuardSession = session;

    KE_LOG_INFO() << "startPageGuardWatchAsync(address=0x" << std::hex << address << std::dec
                  << ", size=" << config.size
                  << ", timeoutMs=" << config.timeoutMs
                  << ", maxHits=" << config.maxHits
                  << ", requestId=" << requestId << ")";

    std::thread([self, requestId, requestedAddress, config, session, pid]() {
        // ProcessHandle n'est pas copiable (RAII autour d'un HANDLE) — on en
        // rouvre un propre à ce thread plutôt que de partager celui de
        // DebugFeatureManager::m_handle entre threads. AllAccess est requis
        // ici (contrairement au ReadWrite habituel) : l'injection de la DLL
        // handler passe par VirtualAllocEx/WriteProcessMemory/CreateRemoteThread.
        killcore::ProcessHandle ownedHandle(pid, killcore::ProcessAccess::AllAccess);
        if (!ownedHandle.isValid()) {
            if (!self) return;
            QMetaObject::invokeMethod(self.data(), [self, requestId, requestedAddress, config]() {
                if (!self) return;
                QVariantMap finished;
                finished["requestId"] = requestId;
                finished["kind"] = "page_guard_watch";
                finished["success"] = false;
                finished["address"] = requestedAddress;
                finished["hits"] = QVariantList();
                finished["hitCount"] = 0;
                finished["error"] = KE_TXT("Impossible d'ouvrir le processus avec les droits nécessaires à l'injection (PROCESS_ALL_ACCESS).", "Could not open the process with the rights required for injection (PROCESS_ALL_ACCESS).");
                self->m_pageGuardWatchInProgress = false;
                self->m_activePageGuardSession.reset();
                self->m_pageGuardWatchFinished(finished);
            }, Qt::QueuedConnection);
            return;
        }

        const auto pageResult = session->monitor(ownedHandle, config);

        if (!self) {
            return;
        }

        QMetaObject::invokeMethod(self.data(), [self, requestId, requestedAddress, config, pageResult]() {
            if (!self) {
                return;
            }

            QVariantList hitList;
            for (const auto& hit : pageResult.hits) {
                QVariantMap item;
                item["address"] = QString::number(hit.accessAddress, 16).toUpper();
                item["instructionPointer"] = QString::number(hit.instructionPointer, 16).toUpper();
                item["threadId"] = static_cast<qulonglong>(hit.threadId);
                item["isWrite"] = hit.isWrite;
                item["module"] = hit.module;
                item["moduleOffset"] = QString::number(hit.moduleOffset, 16).toUpper();
                hitList.append(item);
            }

            QVariantMap finished;
            finished["requestId"] = requestId;
            finished["kind"] = "page_guard_watch";
            finished["success"] = pageResult.success;
            finished["address"] = requestedAddress;
            finished["hits"] = hitList;
            finished["hitCount"] = hitList.size();
            finished["size"] = static_cast<int>(config.size);
            finished["timeoutMs"] = config.timeoutMs;
            finished["maxHits"] = static_cast<int>(config.maxHits);
            finished["timedOut"] = pageResult.timedOut;
            finished["warning"] = KE_TXT("Capture par PAGE_GUARD (sans canal de debug Win32) : moins précise qu'un hardware breakpoint (granularité page de 4 Ko, hits rapprochés potentiellement fusionnés).", "Capture via PAGE_GUARD (no Win32 debug channel): less precise than a hardware breakpoint (4 KB page granularity, closely-spaced hits potentially merged).");
            finished["error"] = !pageResult.success
                ? pageResult.error
                : pageResult.hits.isEmpty()
                ? KE_TXT("Aucun accès capturé pendant la fenêtre d'observation.", "No access captured during the observation window.")
                : QString();

            self->m_pageGuardWatchInProgress = false;
            self->m_activePageGuardSession.reset();
            KE_LOG_INFO() << "pageGuardWatchFinished(requestId=" << requestId
                          << ", success=" << pageResult.success
                          << ", hits=" << hitList.size()
                          << ", error=" << finished["error"].toString().toStdString()
                          << ")";
            self->m_pageGuardWatchFinished(finished);
        }, Qt::QueuedConnection);
    }).detach();

    result["success"] = true;
    result["started"] = true;
    result["requestId"] = requestId;
    result["size"] = static_cast<int>(config.size);
    result["timeoutMs"] = config.timeoutMs;
    result["maxHits"] = static_cast<int>(config.maxHits);
    result["error"] = "";
    return result;
}

QVariantMap DebugFeatureManager::cancelPageGuardWatch() {
    QVariantMap result;
    result["success"] = false;
    if (!m_pageGuardWatchInProgress || !m_activePageGuardSession) {
        result["error"] = KE_TXT("Aucune capture Page Guard active à annuler.", "No active Page Guard capture to cancel.");
        return result;
    }

    m_activePageGuardSession->stop();
    result["success"] = true;
    result["error"] = "";
    return result;
}

// Roadmap section F, niveau 2 (docs/POWER_UP_ROADMAP.md, docs/STRATEGY_ROOM.md) :
// breakpoint materiel pose depuis un composant charge DANS la cible, sans
// jamais attacher de debugger externe — meme structure que
// startPageGuardWatchAsync ci-dessus, avec InProcessBreakpointSession.
QVariantMap DebugFeatureManager::startInProcessBreakpointWatchAsync(const QString& addressHex, const QVariantMap& options) {
    QVariantMap result;
    result["success"] = false;
    result["started"] = false;
    result["address"] = addressHex;

    if (m_inProcessBreakpointWatchInProgress) {
        result["error"] = KE_TXT("Une capture breakpoint in-process est déjà en cours.", "An in-process breakpoint capture is already in progress.");
        return result;
    }
    if (!m_isAttached() || m_pid() <= 0 || !m_handle.isValid()) {
        result["error"] = KE_TXT("Aucun processus attaché.", "No process attached.");
        return result;
    }

    uint64_t address = 0;
    if (!parseHexAddress(addressHex, &address)) {
        result["error"] = KE_TXT("Adresse invalide.", "Invalid address.");
        return result;
    }

    const QString handlerPath = resolveInProcessBreakpointHandlerPath();
    if (handlerPath.isEmpty()) {
        result["error"] = KE_TXT("KillEngineInProcessBreakpointHandler.dll introuvable à côté de KillEngine.exe.", "KillEngineInProcessBreakpointHandler.dll not found next to KillEngine.exe.");
        return result;
    }

    killcore::InProcessBreakpointConfig config;
    config.address = address;
    const int requestedSize = options.value("size", 4).toInt();
    config.size = (requestedSize == 1 || requestedSize == 2 || requestedSize == 8) ? static_cast<size_t>(requestedSize) : 4;
    config.captureWrites = options.value("captureWrites", true).toBool();
    config.captureExecute = options.value("captureExecute", false).toBool()
        || options.value("breakpointType").toString().compare("execute", Qt::CaseInsensitive) == 0;
    if (config.captureExecute) {
        config.size = 1;
        config.captureWrites = false;
    }
    config.timeoutMs = std::clamp(options.value("timeoutMs", 5000).toInt(), 250, 15000);
    config.maxHits = static_cast<size_t>(std::clamp(options.value("maxHits", 10).toInt(), 1, 100));
    config.injectedHandlerPath = handlerPath;
    config.armExistingThreads = options.value("armExistingThreads", false).toBool();

    const int requestId = m_nextRequestId();
    const QString requestedAddress = addressHex;
    const QPointer<DebugFeatureManager> self(this);
    auto session = std::make_shared<killcore::InProcessBreakpointSession>();
    const uint32_t pid = m_pid();

    m_inProcessBreakpointWatchInProgress = true;
    m_activeInProcessBreakpointSession = session;

    KE_LOG_INFO() << "startInProcessBreakpointWatchAsync(address=0x" << std::hex << address << std::dec
                  << ", size=" << config.size
                  << ", timeoutMs=" << config.timeoutMs
                  << ", maxHits=" << config.maxHits
                  << ", requestId=" << requestId
                  << ", self=" << static_cast<void*>(this)
                  << ", session=" << session.get() << ")";

    std::thread([self, requestId, requestedAddress, config, session, pid]() {
        killcore::ProcessHandle ownedHandle(pid, killcore::ProcessAccess::AllAccess);
        if (!ownedHandle.isValid()) {
            if (!self) return;
            QMetaObject::invokeMethod(self.data(), [self, requestId, requestedAddress, config]() {
                if (!self) return;
                QVariantMap finished;
                finished["requestId"] = requestId;
                finished["kind"] = "inprocess_breakpoint_watch";
                finished["success"] = false;
                finished["address"] = requestedAddress;
                finished["hits"] = QVariantList();
                finished["hitCount"] = 0;
                finished["error"] = KE_TXT("Impossible d'ouvrir le processus avec les droits nécessaires à l'injection (PROCESS_ALL_ACCESS).", "Could not open the process with the rights required for injection (PROCESS_ALL_ACCESS).");
                self->m_inProcessBreakpointWatchInProgress = false;
                self->m_activeInProcessBreakpointSession.reset();
                self->m_inProcessBreakpointWatchFinished(finished);
            }, Qt::QueuedConnection);
            return;
        }

        const auto captureResult = session->monitor(ownedHandle, config);

        if (!self) {
            return;
        }

        QMetaObject::invokeMethod(self.data(), [self, requestId, requestedAddress, config, captureResult]() {
            if (!self) {
                return;
            }

            QVariantList hitList;
            for (const auto& hit : captureResult.hits) {
                hitList.append(inProcessBreakpointHitToVariant(hit));
            }

            QVariantMap finished;
            finished["requestId"] = requestId;
            finished["kind"] = "inprocess_breakpoint_watch";
            finished["success"] = captureResult.success;
            finished["address"] = requestedAddress;
            finished["hits"] = hitList;
            finished["hitCount"] = hitList.size();
            finished["size"] = static_cast<int>(config.size);
            finished["captureExecute"] = config.captureExecute;
            finished["timeoutMs"] = config.timeoutMs;
            finished["maxHits"] = static_cast<int>(config.maxHits);
            finished["timedOut"] = captureResult.timedOut;
            finished["existingThreadsArmed"] = captureResult.existingThreadsArmed;
            finished["warning"] = config.armExistingThreads
                ? KE_TXT("Threads préexistantes armées en plus de l'installation: %1.", "Pre-existing threads armed in addition to the install: %1.").arg(captureResult.existingThreadsArmed)
                : KE_TXT(
                      "Seules les threads créées après l'injection sont couvertes — "
                      "une écriture qui vient d'une thread déjà active au moment de "
                      "l'installation peut ne pas être capturée. Passe armExistingThreads=true, "
                      "ou réessaie, ou utilise Find What Writes (débogueur externe).",
                      "Only threads created after the injection are covered — "
                      "a write coming from a thread already active at the time of "
                      "the install may not be captured. Pass armExistingThreads=true, "
                      "retry, or use Find What Writes (external debugger).");
            finished["error"] = !captureResult.success
                ? captureResult.error
                : captureResult.hits.isEmpty()
                ? KE_TXT("Aucune écriture capturée pendant la fenêtre d'observation.", "No write captured during the observation window.")
                : QString();

            self->m_inProcessBreakpointWatchInProgress = false;
            self->m_activeInProcessBreakpointSession.reset();
            self->m_inProcessBreakpointWatchFinished(finished);
        }, Qt::QueuedConnection);
    }).detach();

    result["success"] = true;
    result["started"] = true;
    result["requestId"] = requestId;
    result["size"] = static_cast<int>(config.size);
    result["timeoutMs"] = config.timeoutMs;
    result["maxHits"] = static_cast<int>(config.maxHits);
    result["error"] = "";
    return result;
}

QVariantMap DebugFeatureManager::startInProcessExecuteWatchAsync(const QString& instructionAddressHex, const QVariantMap& options) {
    QVariantMap executeOptions = options;
    executeOptions["captureExecute"] = true;
    executeOptions["captureWrites"] = false;
    executeOptions["size"] = 1;
    const QVariantMap started = startInProcessBreakpointWatchAsync(instructionAddressHex, executeOptions);
    return started;
}

QVariantMap DebugFeatureManager::startInProcessExecuteWatch(const QString& instructionAddressHex, const QVariantMap& options) {
    // Version bloquante de startInProcessExecuteWatchAsync -- meme raison
    // d'etre que findWhatWrites vs findWhatWritesAsync : le pipe d'automation
    // ne peut pas relire le resultat d'un signal Qt asynchrone
    // (inProcessBreakpointWatchFinished), donc un agent pilotant KillEngine
    // via le pipe n'a aucun moyen de recuperer le resultat de la version
    // async. Celle-ci bloque l'appelant jusqu'a un hit ou le timeout, comme
    // findWhatWrites.
    QVariantMap result;
    result["success"] = false;
    result["address"] = instructionAddressHex;

    if (m_inProcessBreakpointWatchInProgress) {
        result["error"] = KE_TXT("Une capture breakpoint in-process est déjà en cours.", "An in-process breakpoint capture is already in progress.");
        return result;
    }
    if (!m_isAttached() || m_pid() <= 0 || !m_handle.isValid()) {
        result["error"] = KE_TXT("Aucun processus attaché.", "No process attached.");
        return result;
    }

    uint64_t address = 0;
    if (!parseHexAddress(instructionAddressHex, &address)) {
        result["error"] = KE_TXT("Adresse invalide.", "Invalid address.");
        return result;
    }

    const QString handlerPath = resolveInProcessBreakpointHandlerPath();
    if (handlerPath.isEmpty()) {
        result["error"] = KE_TXT("KillEngineInProcessBreakpointHandler.dll introuvable à côté de KillEngine.exe.", "KillEngineInProcessBreakpointHandler.dll not found next to KillEngine.exe.");
        return result;
    }

    killcore::InProcessBreakpointConfig config;
    config.address = address;
    config.size = 1;
    config.captureWrites = false;
    config.captureExecute = true;
    config.timeoutMs = std::clamp(options.value("timeoutMs", 5000).toInt(), 250, 15000);
    config.maxHits = static_cast<size_t>(std::clamp(options.value("maxHits", 10).toInt(), 1, 100));
    config.injectedHandlerPath = handlerPath;
    config.armExistingThreads = options.value("armExistingThreads", false).toBool();

    killcore::ProcessHandle ownedHandle(static_cast<uint32_t>(m_pid()), killcore::ProcessAccess::AllAccess);
    if (!ownedHandle.isValid()) {
        result["error"] = KE_TXT("Impossible d'ouvrir le processus avec les droits nécessaires à l'injection (PROCESS_ALL_ACCESS).", "Could not open the process with the rights required for injection (PROCESS_ALL_ACCESS).");
        return result;
    }

    m_inProcessBreakpointWatchInProgress = true;
    auto session = std::make_shared<killcore::InProcessBreakpointSession>();
    m_activeInProcessBreakpointSession = session;

    KE_LOG_INFO() << "startInProcessExecuteWatch(address=0x" << std::hex << address << std::dec
                  << ", timeoutMs=" << config.timeoutMs << ", maxHits=" << config.maxHits
                  << ", armExistingThreads=" << config.armExistingThreads << ")";

    const auto captureResult = session->monitor(ownedHandle, config);

    m_inProcessBreakpointWatchInProgress = false;
    m_activeInProcessBreakpointSession.reset();

    QVariantList hitList;
    for (const auto& hit : captureResult.hits) {
        hitList.append(inProcessBreakpointHitToVariant(hit));
    }

    result["success"] = captureResult.success;
    result["hits"] = hitList;
    result["hitCount"] = hitList.size();
    result["timedOut"] = captureResult.timedOut;
    result["timeoutMs"] = config.timeoutMs;
    result["maxHits"] = static_cast<int>(config.maxHits);
    result["existingThreadsArmed"] = captureResult.existingThreadsArmed;
    result["error"] = !captureResult.success
        ? captureResult.error
        : captureResult.hits.isEmpty()
        ? KE_TXT("Aucune exécution capturée pendant la fenêtre d'observation.", "No execution captured during the observation window.")
        : QString();
    return result;
}

QVariantMap DebugFeatureManager::cancelInProcessBreakpointWatch() {
    QVariantMap result;
    result["success"] = false;
    if (!m_inProcessBreakpointWatchInProgress || !m_activeInProcessBreakpointSession) {
        result["error"] = KE_TXT("Aucune capture breakpoint in-process active à annuler.", "No active in-process breakpoint capture to cancel.");
        return result;
    }

    m_activeInProcessBreakpointSession->stop();
    result["success"] = true;
    result["error"] = "";
    return result;
}

QVariantMap DebugFeatureManager::startInProcessBreakpointFreeze(const QString& addressHex, const QString& valueType, const QString& value, const QVariantMap& options) {
    QVariantMap result;
    result["success"] = false;
    result["enabled"] = false;
    result["mode"] = "inprocess";

    if (!m_isAttached() || m_pid() <= 0) {
        result["error"] = KE_TXT("Aucun processus attaché.", "No process attached.");
        return result;
    }
    if (m_inProcessBreakpointFreezeSession && m_inProcessBreakpointFreezeSession->isFreezing()) {
        result["error"] = KE_TXT("Un freeze breakpoint in-process est déjà actif — arrête-le avant d'en démarrer un autre.", "An in-process breakpoint freeze is already active — stop it before starting another one.");
        return result;
    }

    uint64_t address = 0;
    if (!parseHexAddress(addressHex, &address)) {
        result["error"] = KE_TXT("Adresse invalide.", "Invalid address.");
        return result;
    }

    killcore::ValueType type;
    if (!killcore::parseValueType(valueType, &type)) {
        result["error"] = KE_TXT("Type invalide.", "Invalid type.");
        return result;
    }

    killcore::ScanValue scanValue;
    QString parseError;
    if (!killcore::parseScanValue(value, type, &scanValue, &parseError)) {
        result["error"] = parseError;
        return result;
    }

    const QByteArray frozenBytes = killcore::scanValueToBytes(scanValue);
    if (frozenBytes.size() > 8) {
        result["error"] = KE_TXT("Type trop large pour un freeze breakpoint in-process (8 octets maximum).", "Type too large for an in-process breakpoint freeze (8 bytes maximum).");
        return result;
    }

    const QString handlerPath = resolveInProcessBreakpointHandlerPath();
    if (handlerPath.isEmpty()) {
        result["error"] = KE_TXT("KillEngineInProcessBreakpointHandler.dll introuvable à côté de KillEngine.exe.", "KillEngineInProcessBreakpointHandler.dll not found next to KillEngine.exe.");
        return result;
    }

    killcore::ProcessHandle ownedHandle(static_cast<uint32_t>(m_pid()), killcore::ProcessAccess::AllAccess);
    if (!ownedHandle.isValid()) {
        result["error"] = KE_TXT("Impossible d'ouvrir le processus avec les droits nécessaires à l'injection (PROCESS_ALL_ACCESS).", "Could not open the process with the rights required for injection (PROCESS_ALL_ACCESS).");
        return result;
    }

    const bool captureWrites = options.value("captureWrites", true).toBool();
    auto session = std::make_shared<killcore::InProcessBreakpointSession>();
    QString startError;
    if (!session->startFreeze(ownedHandle, address, frozenBytes.size(), captureWrites, frozenBytes, handlerPath, &startError)) {
        result["error"] = startError;
        return result;
    }

    m_inProcessBreakpointFreezeSession = session;
    result["success"] = true;
    result["enabled"] = true;
    result["armedThreadCount"] = session->freezeStats().armedThreadCount;
    result["warning"] = KE_TXT(
        "Seules les threads créées après l'injection sont couvertes pour l'instant — "
        "si l'écriture vient d'une thread déjà active au moment de l'installation, "
        "le freeze peut ne pas tenir. Si ça ne tient pas, réessaie (une nouvelle "
        "injection réarme la thread appelante) ou utilise le freeze par breakpoint "
        "externe classique.",
        "Only threads created after the injection are covered for now — "
        "if the write comes from a thread already active at the time of the install, "
        "the freeze may not hold. If it doesn't hold, retry (a new "
        "injection re-arms the calling thread) or use the classic "
        "external breakpoint freeze instead.");
    KE_LOG_INFO() << "startInProcessBreakpointFreeze(address=0x" << std::hex << address << std::dec
                  << ", type=" << valueType.toStdString() << ", value=" << value.toStdString() << ")";
    return result;
}

QVariantMap DebugFeatureManager::stopInProcessBreakpointFreeze() {
    QVariantMap result;
    result["success"] = true;
    result["enabled"] = false;
    result["mode"] = "inprocess";

    if (m_inProcessBreakpointFreezeSession) {
        const auto stats = m_inProcessBreakpointFreezeSession->freezeStats();
        m_inProcessBreakpointFreezeSession->stop();
        result["hits"] = static_cast<qulonglong>(stats.hitCount);
        result["rewrites"] = static_cast<qulonglong>(stats.hitCount);
        m_inProcessBreakpointFreezeSession.reset();
    }
    return result;
}

QVariantMap DebugFeatureManager::getInProcessBreakpointFreezeStats() const {
    QVariantMap result;
    result["mode"] = "inprocess";
    if (!m_inProcessBreakpointFreezeSession || !m_inProcessBreakpointFreezeSession->isFreezing()) {
        result["active"] = false;
        result["hits"] = 0ULL;
        result["rewrites"] = 0ULL;
        result["armedThreadCount"] = 0;
        return result;
    }

    const auto stats = m_inProcessBreakpointFreezeSession->freezeStats();
    result["active"] = stats.active;
    result["hits"] = static_cast<qulonglong>(stats.hitCount);
    result["rewrites"] = static_cast<qulonglong>(stats.hitCount);
    result["armedThreadCount"] = stats.armedThreadCount;
    result["healthy"] = stats.active && !stats.installError;
    return result;
}

QVariantMap DebugFeatureManager::startSpeedhack(double factor) {
    QVariantMap result;
    result["success"] = false;

    if (!m_isAttached() || m_pid() <= 0) {
        result["error"] = KE_TXT("Aucun processus attaché.", "No process attached.");
        return result;
    }
    if (m_speedhackSession && m_speedhackSession->isActive()) {
        result["error"] = KE_TXT("Un speedhack est déjà actif — change le facteur au lieu d'en redémarrer un.", "A speedhack is already active — change the factor instead of restarting one.");
        return result;
    }

    const QString handlerPath = resolveSpeedhackHandlerPath();
    if (handlerPath.isEmpty()) {
        result["error"] = KE_TXT("KillEngineSpeedhackHandler.dll introuvable à côté de KillEngine.exe.", "KillEngineSpeedhackHandler.dll not found next to KillEngine.exe.");
        return result;
    }

    killcore::ProcessHandle ownedHandle(static_cast<uint32_t>(m_pid()), killcore::ProcessAccess::AllAccess);
    if (!ownedHandle.isValid()) {
        result["error"] = KE_TXT("Impossible d'ouvrir le processus avec les droits nécessaires à l'injection (PROCESS_ALL_ACCESS).", "Could not open the process with the rights required for injection (PROCESS_ALL_ACCESS).");
        return result;
    }

    if (!m_speedhackSession) {
        m_speedhackSession = std::make_unique<killcore::SpeedhackSession>();
    }
    QString startError;
    if (!m_speedhackSession->start(ownedHandle, factor, handlerPath, &startError)) {
        result["error"] = startError;
        return result;
    }

    const auto stats = m_speedhackSession->stats();
    result["success"] = true;
    result["active"] = stats.active;
    result["factor"] = stats.factor;
    result["hooksInstalledMask"] = static_cast<int>(stats.hooksInstalledMask);
    KE_LOG_INFO() << "startSpeedhack(pid=" << m_pid() << ", factor=" << factor << ")";
    return result;
}

QVariantMap DebugFeatureManager::setSpeedhackFactor(double factor) {
    QVariantMap result;
    result["success"] = false;

    if (!m_speedhackSession || !m_speedhackSession->isActive()) {
        result["error"] = KE_TXT("Aucun speedhack actif.", "No active speedhack.");
        return result;
    }
    if (!m_speedhackSession->setFactor(factor)) {
        result["error"] = KE_TXT("Échec du réglage du facteur.", "Failed to set the factor.");
        return result;
    }

    const auto stats = m_speedhackSession->stats();
    result["success"] = true;
    result["active"] = stats.active;
    result["installError"] = stats.installError;
    result["factor"] = stats.factor;
    result["hooksInstalledMask"] = static_cast<int>(stats.hooksInstalledMask);
    result["pid"] = m_pid();
    return result;
}

QVariantMap DebugFeatureManager::stopSpeedhack() {
    QVariantMap result;
    result["success"] = true;
    result["active"] = false;

    if (m_speedhackSession) {
        m_speedhackSession->stop();
    }
    return result;
}

QVariantMap DebugFeatureManager::getSpeedhackStatus() const {
    QVariantMap result;
    if (!m_speedhackSession || !m_speedhackSession->isActive()) {
        result["success"] = true;
        result["active"] = false;
        result["factor"] = 1.0;
        return result;
    }

    const auto stats = m_speedhackSession->stats();
    result["success"] = true;
    result["active"] = stats.active;
    result["installError"] = stats.installError;
    result["factor"] = stats.factor;
    result["hooksInstalledMask"] = static_cast<int>(stats.hooksInstalledMask);
    result["pid"] = m_pid();
    return result;
}

QVariantMap DebugFeatureManager::startApiHook(const QString& moduleName, const QString& functionName,
                                                int mode, qlonglong forcedReturnValue) {
    QVariantMap result;
    result["success"] = false;

    if (!m_isAttached() || m_pid() <= 0) {
        result["error"] = KE_TXT("Aucun processus attaché.", "No process attached.");
        return result;
    }
    if (m_apiHookSession && m_apiHookSession->isActive()) {
        result["error"] = KE_TXT("Une interception est deja active - arrete-la avant d en demarrer une autre.", "An interception is already active - stop it before starting another one.");
        return result;
    }

    const QString handlerPath = resolveApiHookHandlerPath();
    if (handlerPath.isEmpty()) {
        result["error"] = KE_TXT("KillEngineApiHookHandler.dll introuvable a cote de KillEngine.exe.", "KillEngineApiHookHandler.dll not found next to KillEngine.exe.");
        return result;
    }

    killcore::ApiHookConfig config;
    config.moduleName = moduleName;
    config.functionName = functionName;
    config.mode = mode == 1 ? killcore::ApiHookMode::ForceReturn : killcore::ApiHookMode::Count;
    config.forcedReturnValue = static_cast<int64_t>(forcedReturnValue);

    killcore::ProcessHandle ownedHandle(static_cast<uint32_t>(m_pid()), killcore::ProcessAccess::AllAccess);
    if (!ownedHandle.isValid()) {
        result["error"] = KE_TXT("Impossible d'ouvrir le processus avec les droits nécessaires à l'injection (PROCESS_ALL_ACCESS).", "Could not open the process with the rights required for injection (PROCESS_ALL_ACCESS).");
        return result;
    }

    if (!m_apiHookSession) {
        m_apiHookSession = std::make_unique<killcore::ApiHookSession>();
    }
    QString startError;
    if (!m_apiHookSession->start(ownedHandle, config, handlerPath, &startError)) {
        result["error"] = startError;
        return result;
    }

    const auto stats = m_apiHookSession->stats();
    result["success"] = true;
    result["active"] = stats.active;
    result["callCount"] = static_cast<qlonglong>(stats.callCount);
    KE_LOG_INFO() << "startApiHook(pid=" << m_pid() << ", " << moduleName.toStdString()
                  << "!" << functionName.toStdString() << ", mode=" << mode << ")";
    return result;
}

QVariantMap DebugFeatureManager::stopApiHook() {
    QVariantMap result;
    result["success"] = true;
    result["active"] = false;

    if (m_apiHookSession) {
        const auto stats = m_apiHookSession->stats();
        result["finalCallCount"] = static_cast<qlonglong>(stats.callCount);
        m_apiHookSession->stop();
    }
    return result;
}

QVariantMap DebugFeatureManager::getApiHookStatus() const {
    QVariantMap result;
    if (!m_apiHookSession || !m_apiHookSession->isActive()) {
        result["success"] = true;
        result["active"] = false;
        return result;
    }
    const auto stats = m_apiHookSession->stats();
    result["success"] = true;
    result["active"] = stats.active;
    result["installError"] = stats.installError;
    result["resolveError"] = stats.resolveError;
    result["callCount"] = static_cast<qlonglong>(stats.callCount);
    result["pid"] = m_pid();
    return result;
}

QVariantMap DebugFeatureManager::findWhatAccessesAsync(const QString& addressHex, const QVariantMap& options) {
    QVariantMap result;
    result["success"] = false;
    result["started"] = false;
    result["address"] = addressHex;

    if (m_findWhatAccessesInProgress || m_findWhatWritesInProgress) {
        result["error"] = KE_TXT("Une capture debugger est deja en cours.", "A debugger capture is already in progress.");
        return result;
    }
    if (!m_isAttached() || m_pid() <= 0) {
        result["error"] = KE_TXT("Aucun processus attaché.", "No process attached.");
        return result;
    }

    uint64_t address = 0;
    if (!parseHexAddress(addressHex, &address)) {
        result["error"] = KE_TXT("Adresse invalide.", "Invalid address.");
        return result;
    }

    const int sizeBytes = std::clamp(options.value("size", 4).toInt(), 1, 8);
    killcore::BreakpointSize breakpointSize = killcore::BreakpointSize::DWord;
    if (sizeBytes <= 1) {
        breakpointSize = killcore::BreakpointSize::Byte;
    } else if (sizeBytes <= 2) {
        breakpointSize = killcore::BreakpointSize::Word;
    } else if (sizeBytes <= 4) {
        breakpointSize = killcore::BreakpointSize::DWord;
    } else {
        breakpointSize = killcore::BreakpointSize::QWord;
    }

    const int timeoutMs = std::clamp(options.value("timeoutMs", 5000).toInt(), 250, 15000);
    const int maxHitsInt = std::clamp(options.value("maxHits", 10).toInt(), 1, 100);
    const int requestId = m_nextRequestId();
    const int pid = m_pid();
    const QString requestedAddress = addressHex;
    const QPointer<DebugFeatureManager> self(this);
    auto cancellation = std::make_shared<killcore::CancellationToken>();

    m_findWhatAccessesInProgress = true;
    m_activeDebugCancellation = cancellation;

    KE_LOG_INFO() << "findWhatAccessesAsync(address=0x" << std::hex << address
                  << ", pid=" << std::dec << pid
                  << ", size=" << sizeBytes
                  << ", timeoutMs=" << timeoutMs
                  << ", maxHits=" << maxHitsInt
                  << ", requestId=" << requestId << ")";

    std::thread([self, requestId, pid, address, requestedAddress, breakpointSize, sizeBytes, timeoutMs, maxHitsInt, cancellation]() {
        const auto hits = killcore::findWhatAccesses(
            static_cast<uint32_t>(pid),
            address,
            breakpointSize,
            timeoutMs,
            static_cast<size_t>(maxHitsInt),
            cancellation.get());
        const bool cancelled = cancellation->isCancelled();

        if (!self) {
            return;
        }

        QMetaObject::invokeMethod(self.data(), [self, requestId, requestedAddress, sizeBytes, timeoutMs, maxHitsInt, hits, cancelled]() {
            if (!self) {
                return;
            }

            QVariantList hitList;
            for (const auto& hit : hits) {
                QVariantMap item;
                item["address"] = QString::number(hit.address, 16).toUpper();
                item["instructionPointer"] = QString::number(hit.instructionPointer, 16).toUpper();
                item["threadId"] = static_cast<qulonglong>(hit.threadId);
                item["valueBefore"] = static_cast<qulonglong>(hit.valueBefore);
                item["valueAfter"] = static_cast<qulonglong>(hit.valueAfter);
                item["module"] = hit.module;
                item["moduleOffset"] = QString::number(hit.moduleOffset, 16).toUpper();
                hitList.append(item);
            }

            QVariantMap finished;
            finished["requestId"] = requestId;
            finished["kind"] = "find_what_accesses";
            finished["success"] = true;
            finished["address"] = requestedAddress;
            finished["hits"] = hitList;
            finished["hitCount"] = hitList.size();
            finished["size"] = sizeBytes;
            finished["timeoutMs"] = timeoutMs;
            finished["maxHits"] = maxHitsInt;
            finished["cancelled"] = cancelled;
            finished["warning"] = KE_TXT("Cette fonction attache KillEngine comme debugger au processus cible pendant la capture.", "This function attaches KillEngine as a debugger to the target process during the capture.");
            finished["error"] = cancelled
                ? KE_TXT("Capture Find What Accesses annulee.", "Find What Accesses capture cancelled.")
                : hits.isEmpty()
                ? KE_TXT("Aucun acces capture pendant la fenetre d'observation.", "No access captured during the observation window.")
                : QString();

            self->m_findWhatAccessesInProgress = false;
            self->m_activeDebugCancellation.reset();
            self->m_findWhatAccessesFinished(finished);
        }, Qt::QueuedConnection);
    }).detach();

    result["success"] = true;
    result["started"] = true;
    result["requestId"] = requestId;
    result["size"] = sizeBytes;
    result["timeoutMs"] = timeoutMs;
    result["maxHits"] = maxHitsInt;
    result["error"] = "";
    return result;
}

QVariantMap DebugFeatureManager::disassembleBackward(const QString& addressHex, const QVariantMap& options) const {
    QVariantMap result;
    result["success"] = false;
    result["address"] = addressHex;

    if (!m_isAttached() || !m_handle.isValid()) {
        result["error"] = KE_TXT("Aucun processus attaché.", "No process attached.");
        return result;
    }

    uint64_t address = 0;
    if (!parseHexAddress(addressHex, &address)) {
        result["error"] = KE_TXT("Adresse invalide.", "Invalid address.");
        return result;
    }

    const int windowBytes = std::clamp(options.value("windowBytes", 64).toInt(), 16, 128);
    const int trailingBytes = 16; // Marge pour décoder entièrement l'instruction cible elle-même.
    const uint64_t start = address >= static_cast<uint64_t>(windowBytes) ? address - static_cast<uint64_t>(windowBytes) : 0;
    const int targetOffsetInWindow = static_cast<int>(address - start);

    killcore::MemoryReader reader(m_handle);
    const auto read = reader.readChunked(start, static_cast<size_t>(targetOffsetInWindow + trailingBytes), 4096);
    if (!read.success && read.bytesRead == 0) {
        const QString hint = codeReadProtectionHint(read.errorCode);
        if (!hint.isEmpty()) {
            result["error"] = hint;
            result["codeReadProtected"] = true;
        } else {
            result["error"] = read.errorMessage.isEmpty() ? KE_TXT("Lecture mémoire impossible.", "Memory read failed.") : read.errorMessage;
        }
        return result;
    }
    if (targetOffsetInWindow > read.data.size()) {
        result["error"] = KE_TXT("Lecture mémoire trop courte pour atteindre l'adresse cible.", "Memory read too short to reach the target address.");
        return result;
    }

    const auto backward = killcore::disassembleBackwardWindow(read.data, targetOffsetInWindow);
    result["success"] = backward.success;
    result["error"] = backward.error;
    if (!backward.success) {
        return result;
    }

    QVariantList instructions;
    QVariantList candidateFields;
    uint64_t cursor = start + static_cast<uint64_t>(backward.startOffsetInWindow);
    for (const auto& info : backward.instructions) {
        QVariantMap item;
        const QString instrAddressHex = QString::number(cursor, 16).toUpper();
        item["address"] = instrAddressHex;
        item["bytes"] = info.rawBytesText;
        item["disassembly"] = info.disassembly;
        item["mnemonicHint"] = info.mnemonicHint;
        item["category"] = info.category;
        item["memBaseRegister"] = info.memBaseRegister;
        item["memDisplacement"] = static_cast<qlonglong>(info.memDisplacement);
        const bool isCandidateField = !info.memBaseRegister.isEmpty();
        item["isCandidateField"] = isCandidateField;
        instructions.append(item);
        if (isCandidateField) {
            QVariantMap candidate = item;
            candidateFields.append(candidate);
        }
        cursor += static_cast<uint64_t>(info.length);
    }

    result["instructions"] = instructions;
    result["candidateFields"] = candidateFields;
    result["warning"] = KE_TXT("Désassemblage en arrière expérimental (lecture seule). Vérifie toujours les champs candidats avant d'écrire dessus.", "Experimental backward disassembly (read-only). Always verify the candidate fields before writing to them.");
    m_appendScanTelemetry("disassemble_backward", result);
    return result;
}

QVariantMap DebugFeatureManager::freezeWithBreakpoint(const QString& addressHex, const QString& valueType, const QString& value, const QVariantMap& options) {
    QVariantMap result;
    result["success"] = false;
    result["enabled"] = false;
    result["mode"] = "breakpoint";

    if (!m_isAttached() || m_pid() <= 0) {
        result["error"] = KE_TXT("Aucun processus attaché.", "No process attached.");
        return result;
    }

    uint64_t address = 0;
    if (!parseHexAddress(addressHex, &address)) {
        result["error"] = KE_TXT("Adresse invalide.", "Invalid address.");
        return result;
    }

    killcore::ValueType type;
    if (!killcore::parseValueType(valueType, &type)) {
        result["error"] = KE_TXT("Type invalide.", "Invalid type.");
        return result;
    }

    killcore::ScanValue scanValue;
    QString parseError;
    if (!killcore::parseScanValue(value, type, &scanValue, &parseError)) {
        result["error"] = parseError;
        return result;
    }

    const QByteArray frozenBytes = killcore::scanValueToBytes(scanValue);
    switch (frozenBytes.size()) {
        case 1:
        case 2:
        case 4:
        case 8:
            break;
        default:
            result["error"] = KE_TXT("Taille de valeur incompatible avec un hardware breakpoint.", "Value size incompatible with a hardware breakpoint.");
            return result;
    }

    killcore::BreakpointFreezeMode mode = killcore::BreakpointFreezeMode::RewriteValue;
    const QString modeText = options.value("mode", "rewrite").toString().toLower();
    if (modeText == "capture") {
        mode = killcore::BreakpointFreezeMode::Capture;
    } else if (modeText == "block" || modeText == "blockwrite") {
        mode = killcore::BreakpointFreezeMode::BlockWrite;
    }

    return activateBreakpointFreezeFor(address, type, frozenBytes, mode, modeText);
}

QVariantMap DebugFeatureManager::activateBreakpointFreezeFor(
    uint64_t address,
    killcore::ValueType type,
    const QByteArray& frozenBytes,
    killcore::BreakpointFreezeMode mode,
    const QString& modeText) {
    QVariantMap result;
    m_freeze.setEntry(address, type, frozenBytes, killcore::FreezeMode::HardwareBreakpoint);
    QString restartError;
    const bool ok = restartBreakpointFreezeFromRegistry(mode, &restartError);
    result["success"] = ok;
    result["enabled"] = ok;
    result["address"] = QString::number(address, 16).toUpper();
    result["type"] = killcore::valueTypeToString(type);
    result["bytesWritten"] = 0;
    result["verified"] = false;
    result["breakpointSize"] = frozenBytes.size();
    result["freezeMode"] = modeText;
    result["mode"] = "breakpoint";
    if (!ok) {
        m_freeze.remove(address);
        restartBreakpointFreezeFromRegistry(mode);
        result["error"] = restartError.isEmpty()
            ? KE_TXT("Impossible d'activer le freeze par hardware breakpoint. Vérifie les privilèges debug et la cible.", "Could not activate the hardware breakpoint freeze. Check the debug privileges and the target.")
            : restartError;
    }
    return result;
}

QVariantMap DebugFeatureManager::escalatePollingFreezeToBreakpoint(const QString& addressHex) {
    QVariantMap result;
    result["success"] = false;
    result["enabled"] = false;
    result["mode"] = "breakpoint";

    if (!m_isAttached() || m_pid() <= 0) {
        result["error"] = KE_TXT("Aucun processus attaché.", "No process attached.");
        return result;
    }

    uint64_t address = 0;
    if (!parseHexAddress(addressHex, &address)) {
        result["error"] = KE_TXT("Adresse invalide.", "Invalid address.");
        return result;
    }

    const killcore::FreezeEntry* pollingEntry = nullptr;
    for (const auto& entry : m_freeze.entries()) {
        if (entry.address == address && entry.mode == killcore::FreezeMode::Polling) {
            pollingEntry = &entry;
            break;
        }
    }
    if (!pollingEntry) {
        result["error"] = KE_TXT("Aucun freeze polling actif sur cette adresse (déjà arrêté ou déjà en Freeze BP ?).", "No active polling freeze on this address (already stopped, or already in Freeze BP?).");
        return result;
    }

    // Reutilise directement les bytes/type deja connus de l'entree polling :
    // aucun decodage/re-encodage depuis une chaine, contrairement a
    // freezeWithBreakpoint() qui part d'une saisie utilisateur.
    return activateBreakpointFreezeFor(
        pollingEntry->address,
        pollingEntry->type,
        pollingEntry->value,
        killcore::BreakpointFreezeMode::RewriteValue,
        "rewrite");
}

QVariantMap DebugFeatureManager::stopBreakpointFreezeCommand() {
    QVariantMap result;
    result["success"] = true;
    result["enabled"] = false;
    result["mode"] = "breakpoint";

    if (m_breakpointFreeze) {
        const auto stats = m_breakpointFreeze->stats();
        m_breakpointFreeze->stop();
        m_freeze.removeByMode(killcore::FreezeMode::HardwareBreakpoint);
        result["hits"] = static_cast<qulonglong>(stats.totalHits);
        result["rewrites"] = static_cast<qulonglong>(stats.rewrites);
        result["blocks"] = static_cast<qulonglong>(stats.blocks);
        result["errors"] = static_cast<qulonglong>(stats.errors);
    }
    return result;
}

QVariantMap DebugFeatureManager::getBreakpointFreezeStats() const {
    QVariantMap result;
    result["active"] = m_breakpointFreeze && m_breakpointFreeze->isActive();
    result["mode"] = "breakpoint";
    if (!m_breakpointFreeze) {
        result["hits"] = 0ULL;
        result["rewrites"] = 0ULL;
        result["blocks"] = 0ULL;
        result["errors"] = 0ULL;
        return result;
    }

    const auto stats = m_breakpointFreeze->stats();
    result["hits"] = static_cast<qulonglong>(stats.totalHits);
    result["rewrites"] = static_cast<qulonglong>(stats.rewrites);
    result["blocks"] = static_cast<qulonglong>(stats.blocks);
    result["errors"] = static_cast<qulonglong>(stats.errors);

    // Signal simple, sans nouveau minuteur dédié : si des erreurs de
    // réécriture s'accumulent, le freeze BP n'est probablement pas en train
    // de tenir correctement malgré des hits. L'UI peut afficher ceci sans
    // attendre l'arrêt du freeze.
    result["healthy"] = stats.errors == 0;
    return result;
}

bool DebugFeatureManager::restartBreakpointFreezeFromRegistry(killcore::BreakpointFreezeMode mode, QString* error) {
    if (!m_isAttached() || m_pid() <= 0) {
        if (error) *error = KE_TXT("Aucun processus attaché.", "No process attached.");
        return false;
    }

    const auto entries = m_freeze.entriesForMode(killcore::FreezeMode::HardwareBreakpoint);
    if (entries.isEmpty()) {
        if (m_breakpointFreeze) {
            m_breakpointFreeze->stop();
        }
        return true;
    }

    if (entries.size() > 4) {
        if (error) *error = KE_TXT("Un hardware breakpoint ne peut surveiller que 4 adresses simultanées (DR0-DR3).", "A hardware breakpoint can only watch 4 addresses at once (DR0-DR3).");
        return false;
    }

    QList<killcore::BreakpointFreezeConfig> configs;
    for (const auto& entry : entries) {
        killcore::BreakpointFreezeConfig config;
        config.address = entry.address;
        config.frozenValue = entry.value;
        config.mode = mode;
        switch (entry.value.size()) {
            case 1: config.size = killcore::BreakpointSize::Byte; break;
            case 2: config.size = killcore::BreakpointSize::Word; break;
            case 4: config.size = killcore::BreakpointSize::DWord; break;
            case 8: config.size = killcore::BreakpointSize::QWord; break;
            default:
                if (error) *error = KE_TXT("Taille de valeur incompatible avec un hardware breakpoint.", "Value size incompatible with a hardware breakpoint.");
                return false;
        }
        configs.append(config);
    }

    if (!m_breakpointFreeze) {
        m_breakpointFreeze = std::make_unique<killcore::BreakpointFreezeManager>();
    }

    if (!m_breakpointFreeze->startMulti(static_cast<uint32_t>(m_pid()), configs)) {
        if (error) *error = KE_TXT("Impossible d'activer la session hardware breakpoint.", "Could not activate the hardware breakpoint session.");
        return false;
    }

    return true;
}

QVariantMap DebugFeatureManager::validatePageStability(const QString& addressHex, const QVariantMap& options) const {
    QVariantMap result;
    result["success"] = false;
    result["stable"] = false;

    if (!m_isAttached() || m_pid() <= 0 || !m_handle.isValid()) {
        result["error"] = KE_TXT("Aucun processus attaché.", "No process attached.");
        return result;
    }

    uint64_t address = 0;
    if (!parseHexAddress(addressHex, &address)) {
        result["error"] = KE_TXT("Adresse invalide.", "Invalid address.");
        return result;
    }

    const int readCount = std::clamp(options.value("readCount", 3).toInt(), 2, 10);
    const int intervalMs = std::clamp(options.value("intervalMs", 100).toInt(), 50, 2000);
    const int pageSize = std::clamp(options.value("pageSize", 4096).toInt(), 1, 65536);

    killcore::MemoryReader reader(m_handle);
    QList<QByteArray> snapshots;
    int unreadable = 0;

    for (int i = 0; i < readCount; ++i) {
        if (i > 0) {
            QThread::msleep(intervalMs);
        }
        const auto read = reader.read(address, static_cast<size_t>(pageSize));
        if (!read.success && !read.partial) {
            ++unreadable;
            snapshots.append(QByteArray());
        } else {
            snapshots.append(read.data.left(static_cast<qsizetype>(read.bytesRead)));
        }
    }

    if (unreadable > 0) {
        result["stable"] = false;
        result["readCount"] = readCount;
        result["unreadable"] = unreadable;
        result["changeCount"] = 0;
        result["reason"] = KE_TXT("Page illisible lors de %1/%2 lectures.", "Page unreadable during %1/%2 reads.").arg(unreadable).arg(readCount);
        result["success"] = true;
        return result;
    }

    int changeCount = 0;
    for (int i = 1; i < snapshots.size(); ++i) {
        if (snapshots[i] != snapshots[0]) {
            ++changeCount;
        }
    }

    const bool stable = (changeCount == 0);
    result["success"] = true;
    result["stable"] = stable;
    result["readCount"] = readCount;
    result["unreadable"] = 0;
    result["changeCount"] = changeCount;
    result["reason"] = stable
        ? KE_TXT("Page stable sur %1 lectures (aucune variation détectée).", "Page stable across %1 reads (no variation detected).").arg(readCount)
        : KE_TXT("Page instable : %1 variation(s) sur %2 lectures.", "Page unstable: %1 variation(s) across %2 reads.").arg(changeCount).arg(readCount);
    return result;
}

} // namespace killengine
