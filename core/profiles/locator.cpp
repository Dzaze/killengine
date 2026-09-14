#include "locator.h"
#include "process/process_enumerator.h"
#include <cstring>
#include <limits>

namespace killcore {

QString Locator::toString() const {
    switch (kind) {
        case LocatorKind::ModuleOffset:
            return QString("%1+0x%2")
                .arg(module)
                .arg(offset, 0, 16);
        case LocatorKind::Absolute:
            return QString("0x%1").arg(lastAddress, 0, 16);
        case LocatorKind::PointerChain:
            return pointerChain.toString();
        case LocatorKind::ClrField:
            return QString("CLR %1[%2=%3].%4")
                .arg(clrField.typeSubstring,
                     clrField.identityField,
                     clrField.identityValue,
                     clrField.targetField);
    }
    return {};
}

bool Locator::isValid() const {
    switch (kind) {
        case LocatorKind::ModuleOffset:
            return !module.isEmpty() && offset > 0;
        case LocatorKind::Absolute:
            return lastAddress > 0;
        case LocatorKind::PointerChain:
            return pointerChain.isValid();
        case LocatorKind::ClrField:
            return clrField.isValid();
    }
    return false;
}

bool resolveLocatorAddress(const ProcessHandle& handle, const Locator& locator, uint64_t* address) {
    if (!address || !handle.isValid()) {
        return false;
    }

    switch (locator.kind) {
        case LocatorKind::Absolute: {
            *address = locator.lastAddress;
            return locator.lastAddress > 0;
        }
        case LocatorKind::ModuleOffset: {
            const auto modules = ProcessEnumerator::enumerateModules(handle.pid());
            for (const auto& mod : modules) {
                if (mod.name.compare(locator.module, Qt::CaseInsensitive) == 0) {
                    *address = mod.baseAddress + locator.offset;
                    return true;
                }
            }
            return false;
        }
        case LocatorKind::PointerChain: {
            const auto result = resolvePointerChain(handle, locator.pointerChain);
            if (!result.success) {
                return false;
            }
            *address = result.finalAddress;
            return true;
        }
        case LocatorKind::ClrField:
            return false;
    }
    return false;
}

LocatorProbe probeLocator(const Locator& locator,
                          const QList<ProcessModuleInfo>& modules,
                          size_t pointerBytes, size_t readBytes,
                          const LocatorReadBytes& read) {
    LocatorProbe result;
    if (!read || readBytes == 0 || readBytes > 64
        || (pointerBytes != 4 && pointerBytes != 8)) {
        result.errorCode = "invalid_probe";
        return result;
    }
    if (locator.kind == LocatorKind::ClrField) {
        result.errorCode = "unsupported_locator";
        return result;
    }
    const uint64_t addressLimit = pointerBytes == 4
        ? std::numeric_limits<uint32_t>::max() : std::numeric_limits<uint64_t>::max();
    uint64_t address = locator.lastAddress;
    if (locator.kind == LocatorKind::ModuleOffset || locator.kind == LocatorKind::PointerChain) {
        const bool chain = locator.kind == LocatorKind::PointerChain;
        const QString moduleName = chain ? locator.pointerChain.module : locator.module;
        const uint64_t offset = chain ? locator.pointerChain.baseOffset : locator.offset;
        const ProcessModuleInfo* module = nullptr;
        for (const auto& candidate : modules) {
            if (candidate.name.compare(moduleName, Qt::CaseInsensitive) == 0) {
                if (module) {
                    result.errorCode = "ambiguous_module";
                    return result;
                }
                module = &candidate;
            }
        }
        if (!module) {
            result.errorCode = "module_missing";
            return result;
        }
        const size_t width = chain ? pointerBytes : readBytes;
        if (module->size < width || offset > module->size - width
            || module->baseAddress > addressLimit || offset > addressLimit - module->baseAddress) {
            result.errorCode = "module_offset_out_of_range";
            return result;
        }
        address = module->baseAddress + offset;
        if (chain) {
            if (locator.pointerChain.offsets.isEmpty() || locator.pointerChain.offsets.size() > 16) {
                result.errorCode = "invalid_pointer_chain";
                return result;
            }
            for (uint64_t nextOffset : locator.pointerChain.offsets) {
                if (address > addressLimit || pointerBytes - 1 > addressLimit - address) {
                    result.errorCode = "address_overflow";
                    return result;
                }
                const QByteArray bytes = read(address, pointerBytes);
                if (bytes.size() != static_cast<qsizetype>(pointerBytes)) {
                    result.errorCode = "pointer_unreadable";
                    return result;
                }
                uint64_t pointer = 0;
                std::memcpy(&pointer, bytes.constData(), pointerBytes);
                if (pointer == 0 || nextOffset > addressLimit - pointer) {
                    result.errorCode = pointer == 0 ? "null_pointer" : "address_overflow";
                    return result;
                }
                address = pointer + nextOffset;
            }
        }
    } else if (locator.kind != LocatorKind::Absolute) {
        result.errorCode = "unsupported_locator";
        return result;
    }
    if (address == 0 || address > addressLimit || readBytes - 1 > addressLimit - address) {
        result.errorCode = "address_overflow";
        return result;
    }
    result.resolved = true;
    result.address = address;
    result.bytes = read(address, readBytes);
    result.readable = result.bytes.size() == static_cast<qsizetype>(readBytes);
    if (!result.readable) result.errorCode = "address_unreadable";
    return result;
}

} // namespace killcore
