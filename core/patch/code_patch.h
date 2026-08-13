#pragma once

#include "process/process_handle.h"

#include <QByteArray>
#include <QString>

#include <cstddef>
#include <cstdint>

namespace killcore {

struct PatchBytes {
    QByteArray bytes;
    QString error;

    bool isValid() const { return error.isEmpty() && !bytes.isEmpty(); }
};

struct CodePatchResult {
    bool success{false};
    bool verified{false};
    bool protectionChanged{false};
    uint64_t address{0};
    size_t bytesWritten{0};
    QByteArray previousBytes;
    QString error;
};

PatchBytes parsePatchBytes(const QString& bytesText);
CodePatchResult applyCodePatch(const ProcessHandle& process, uint64_t address, const QByteArray& patchBytes, bool verify = true);
CodePatchResult restoreCodePatch(const ProcessHandle& process, uint64_t address, const QByteArray& originalBytes, bool verify = true);

} // namespace killcore
