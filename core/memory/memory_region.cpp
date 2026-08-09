#include "memory_region.h"

#ifdef Q_OS_WIN
#include <windows.h>
#endif

#include <QStringList>

namespace killcore {

QString memoryStateToString(MemoryState state) {
    switch (state) {
        case MemoryState::Committed: return "committed";
        case MemoryState::Reserved:  return "reserved";
        case MemoryState::Free:      return "free";
        default:                     return "unknown";
    }
}

QString memoryTypeToString(MemoryType type) {
    switch (type) {
        case MemoryType::Image:   return "image";
        case MemoryType::Mapped:  return "mapped";
        case MemoryType::Private: return "private";
        default:                  return "unknown";
    }
}

QString protectionToString(uint32_t protection) {
#ifdef Q_OS_WIN
    QStringList flags;
    const uint32_t baseProtection = protection & 0xff;

    switch (baseProtection) {
        case PAGE_NOACCESS:          flags << "NOACCESS"; break;
        case PAGE_READONLY:          flags << "R"; break;
        case PAGE_READWRITE:         flags << "RW"; break;
        case PAGE_WRITECOPY:         flags << "WC"; break;
        case PAGE_EXECUTE:           flags << "X"; break;
        case PAGE_EXECUTE_READ:      flags << "XR"; break;
        case PAGE_EXECUTE_READWRITE: flags << "XRW"; break;
        case PAGE_EXECUTE_WRITECOPY: flags << "XWC"; break;
        default:                     flags << "UNKNOWN"; break;
    }

    if ((protection & PAGE_GUARD) != 0) {
        flags << "GUARD";
    }
    if ((protection & PAGE_NOCACHE) != 0) {
        flags << "NOCACHE";
    }
    if ((protection & PAGE_WRITECOMBINE) != 0) {
        flags << "WRITECOMBINE";
    }

    return flags.join("|");
#else
    Q_UNUSED(protection);
    return "UNKNOWN";
#endif
}

} // namespace killcore
