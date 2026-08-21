#pragma once

#include "process/process_handle.h"
#include "memory/memory_reader.h"

#include <QByteArray>
#include <QString>

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace killcore {

// CancellationToken est deja defini dans memory_reader.h (inclus ci-dessus)
// -- ne pas le redefinir ici, une deuxieme definition dans ce header cassait
// la compilation des que les deux headers etaient inclus dans la meme unite
// de traduction (memory_reader.cpp/memory_writer.cpp).

struct MemoryWriteResult {
    bool       success{false};
    bool       verified{false};
    bool       protectionChanged{false};
    bool       cancelled{false}; // Ajoute ce champ
    uint64_t   address{0};
    size_t     requestedBytes{0};
    size_t     bytesWritten{0};
    uint32_t   errorCode{0};
    QString    errorMessage;
    QByteArray previousValue;
};

class MemoryWriter {
public:
    explicit MemoryWriter(const ProcessHandle& process);

    MemoryWriteResult write(uint64_t address, const QByteArray& data, bool verify = true) const;

    MemoryWriteResult writeChunked(
        uint64_t address,
        const QByteArray& data,
        size_t chunkSize = 64 * 1024,
        bool verify = true,
        const CancellationToken* cancellation = nullptr) const;

private:
    const ProcessHandle& m_process;
};

} // namespace killcore