#include "pointer_chain.h"

#include "memory/memory_reader.h"
#include "process/process_enumerator.h"
#include "logging/logger.h"

#include <cstring>

namespace killcore {

QString PointerChain::toString() const {
    QString s = QString("%1+0x%2").arg(module).arg(baseOffset, 0, 16);
    for (uint64_t offset : offsets) {
        s += QString("->0x%1").arg(offset, 0, 16);
    }
    return s;
}

bool PointerChain::isValid() const {
    return !module.isEmpty() && !offsets.isEmpty();
}

PointerChainResolveResult resolvePointerChain(
    const ProcessHandle& handle,
    const PointerChain& chain) {

    PointerChainResolveResult result;

    if (!handle.isValid()) {
        result.errorMessage = "Process handle is not valid.";
        return result;
    }

    if (!chain.isValid()) {
        result.errorMessage = "Pointer chain is not valid.";
        return result;
    }

    // 1. Résoudre la base module -> adresse absolue.
    const auto modules = ProcessEnumerator::enumerateModules(handle.pid());
    uint64_t baseAddress = 0;
    for (const auto& mod : modules) {
        if (mod.name.compare(chain.module, Qt::CaseInsensitive) == 0) {
            baseAddress = mod.baseAddress;
            break;
        }
    }
    if (baseAddress == 0) {
        result.errorMessage = QString("Module '%1' not found in process.").arg(chain.module);
        return result;
    }

    MemoryReader reader(handle);
    uint64_t current = baseAddress + chain.baseOffset;
    result.intermediateAddresses.append(current);

    // 2. Suivre la chaîne de déréférencements.
    // offsets = [n0, n1, ..., nK]
    // Pour chaque offset : on lit un pointeur à current, puis on ajoute l'offset.
    // Une chaîne à un seul offset résout donc read(base+baseOffset)+offset.
    for (int i = 0; i < chain.offsets.size(); ++i) {
        const uint64_t offset = chain.offsets[i];

        // Déréférencement : lire un pointeur (8 octets en x64).
        auto read = reader.read(current, sizeof(uint64_t));
        if (!read.success || read.bytesRead < sizeof(uint64_t)) {
            result.errorMessage = QString("Failed to read pointer at 0x%1 (level %2).")
                                      .arg(current, 0, 16)
                                      .arg(i);
            return result;
        }

        uint64_t ptr = 0;
        std::memcpy(&ptr, read.data.constData(), sizeof(uint64_t));
        if (ptr == 0) {
            result.errorMessage = QString("Null pointer encountered at level %1 (addr 0x%2).")
                                      .arg(i)
                                      .arg(current, 0, 16);
            return result;
        }

        current = ptr + offset;
        result.intermediateAddresses.append(current);
    }

    result.finalAddress = current;
    result.success = true;
    return result;
}

bool pointerChainResolvesTo(
    const ProcessHandle& handle,
    const PointerChain& chain,
    uint64_t expectedAddress) {
    const auto result = resolvePointerChain(handle, chain);
    return result.success && result.finalAddress == expectedAddress;
}

} // namespace killcore
