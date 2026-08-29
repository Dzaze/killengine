#pragma once

#include "process/process_handle.h"
#include "scripting/auto_assembler.h"

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QVariantMap>

#include <cstdint>
#include <functional>
#include <optional>

namespace killengine {

class CodePatchManager {
public:
    using TelemetryCallback = std::function<void(const QString&, const QVariantMap&)>;
    using IsAttachedCallback = std::function<bool()>;
    using PidCallback = std::function<int()>;

    CodePatchManager(
        const killcore::ProcessHandle& handle,
        TelemetryCallback telemetry,
        IsAttachedCallback isAttached,
        PidCallback pid);

    void clearSessionState();
    bool isCodePatchActive(uint64_t address) const;

    QVariantMap scanAobPattern(const QString& patternText, const QVariantMap& optionsMap);
    QVariantMap generateAobSignature(const QString& addressHex, const QVariantMap& options);
    QVariantMap applyCodePatch(const QString& addressHex, const QString& bytesText, const QVariantMap& options);
    QVariantMap suggestCodePatches(const QString& addressHex, const QVariantMap& options);
    QVariantMap restoreCodePatch(const QString& addressHex);
    QVariantMap installFunctionHook(const QString& targetAddressHex, const QString& hookAddressHex);
    QVariantMap removeFunctionHook(const QString& targetAddressHex);
    QVariantMap parseAutoAssemblerScript(const QString& scriptText) const;
    QVariantMap executeAutoAssemblerScript(const QString& scriptText);
    QVariantMap restoreAutoAssemblerScript();

private:
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

    const killcore::ProcessHandle& m_handle;
    TelemetryCallback m_appendScanTelemetry;
    IsAttachedCallback m_isAttached;
    PidCallback m_pid;
    QHash<uint64_t, ActiveCodePatch> m_activeCodePatches;
    QHash<uint64_t, ActiveFunctionHook> m_activeFunctionHooks;
    std::optional<killcore::AutoAsmResult> m_lastAutoAsmResult;
};

} // namespace killengine
