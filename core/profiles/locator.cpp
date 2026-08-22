#include "locator.h"
#include "process/process_enumerator.h"

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

} // namespace killcore
