#include "memory_map.h"

#include "logging/logger.h"

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace killcore {

#ifdef Q_OS_WIN

namespace {

MemoryState toMemoryState(DWORD state) {
    switch (state) {
        case MEM_COMMIT:  return MemoryState::Committed;
        case MEM_RESERVE: return MemoryState::Reserved;
        case MEM_FREE:    return MemoryState::Free;
        default:          return MemoryState::Unknown;
    }
}

MemoryType toMemoryType(DWORD type) {
    switch (type) {
        case MEM_IMAGE:   return MemoryType::Image;
        case MEM_MAPPED:  return MemoryType::Mapped;
        case MEM_PRIVATE: return MemoryType::Private;
        default:          return MemoryType::Unknown;
    }
}

bool isReadableProtection(DWORD protection) {
    if ((protection & PAGE_GUARD) != 0 || (protection & PAGE_NOACCESS) != 0) {
        return false;
    }

    const DWORD baseProtection = protection & 0xff;
    return baseProtection == PAGE_READONLY
        || baseProtection == PAGE_READWRITE
        || baseProtection == PAGE_WRITECOPY
        || baseProtection == PAGE_EXECUTE_READ
        || baseProtection == PAGE_EXECUTE_READWRITE
        || baseProtection == PAGE_EXECUTE_WRITECOPY;
}

bool isWritableProtection(DWORD protection) {
    if ((protection & PAGE_GUARD) != 0 || (protection & PAGE_NOACCESS) != 0) {
        return false;
    }

    const DWORD baseProtection = protection & 0xff;
    return baseProtection == PAGE_READWRITE
        || baseProtection == PAGE_WRITECOPY
        || baseProtection == PAGE_EXECUTE_READWRITE
        || baseProtection == PAGE_EXECUTE_WRITECOPY;
}

bool isExecutableProtection(DWORD protection) {
    if ((protection & PAGE_GUARD) != 0 || (protection & PAGE_NOACCESS) != 0) {
        return false;
    }

    const DWORD baseProtection = protection & 0xff;
    return baseProtection == PAGE_EXECUTE
        || baseProtection == PAGE_EXECUTE_READ
        || baseProtection == PAGE_EXECUTE_READWRITE
        || baseProtection == PAGE_EXECUTE_WRITECOPY;
}

MemoryRegion fromMemoryBasicInformation(const MEMORY_BASIC_INFORMATION& mbi) {
    MemoryRegion region;
    region.baseAddress = reinterpret_cast<uint64_t>(mbi.BaseAddress);
    region.allocationBase = reinterpret_cast<uint64_t>(mbi.AllocationBase);
    region.size = static_cast<uint64_t>(mbi.RegionSize);
    region.protection = static_cast<uint32_t>(mbi.Protect);
    region.allocationProtection = static_cast<uint32_t>(mbi.AllocationProtect);
    region.state = toMemoryState(mbi.State);
    region.type = toMemoryType(mbi.Type);
    region.guarded = (mbi.Protect & PAGE_GUARD) != 0;
    region.readable = region.state == MemoryState::Committed && isReadableProtection(mbi.Protect);
    region.writable = region.state == MemoryState::Committed && isWritableProtection(mbi.Protect);
    region.executable = region.state == MemoryState::Committed && isExecutableProtection(mbi.Protect);
    return region;
}

} // namespace

#endif

QList<MemoryRegion> MemoryMap::snapshot(const ProcessHandle& process) {
    QList<MemoryRegion> regions;

#ifdef Q_OS_WIN
    if (!process.isValid()) {
        return regions;
    }

    SYSTEM_INFO systemInfo;
    GetSystemInfo(&systemInfo);

    auto address = reinterpret_cast<uintptr_t>(systemInfo.lpMinimumApplicationAddress);
    const auto maxAddress = reinterpret_cast<uintptr_t>(systemInfo.lpMaximumApplicationAddress);

    while (address < maxAddress) {
        MEMORY_BASIC_INFORMATION mbi;
        const SIZE_T bytes = VirtualQueryEx(
            process.rawHandle(),
            reinterpret_cast<LPCVOID>(address),
            &mbi,
            sizeof(mbi));

        if (bytes == 0) {
            address += systemInfo.dwPageSize;
            continue;
        }

        regions.append(fromMemoryBasicInformation(mbi));

        const auto regionBase = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const auto nextAddress = regionBase + static_cast<uintptr_t>(mbi.RegionSize);
        if (nextAddress <= address) {
            break;
        }
        address = nextAddress;
    }

    KE_LOG_DEBUG() << "MemoryMap: captured " << regions.size()
                   << " regions for PID " << process.pid();
#else
    Q_UNUSED(process);
#endif

    return regions;
}

MemoryMapStats MemoryMap::stats(const QList<MemoryRegion>& regions) {
    MemoryMapStats result;
    result.regionCount = regions.size();

    for (const auto& region : regions) {
        result.totalBytes += region.size;

        if (region.state == MemoryState::Committed) {
            ++result.committedCount;
            result.committedBytes += region.size;
        }
        if (region.readable) {
            ++result.readableCount;
            result.readableBytes += region.size;
        }
        if (region.writable) {
            ++result.writableCount;
            result.writableBytes += region.size;
        }
        if (region.executable) {
            ++result.executableCount;
            result.executableBytes += region.size;
        }
    }

    return result;
}

} // namespace killcore
