#include "job_object.h"

#include "../logging/logger.h"

#include <QtGlobal>

namespace killcore {

#ifdef Q_OS_WIN

JobObject::JobObject() {
    m_handle = CreateJobObjectW(nullptr, nullptr);
    if (!m_handle) {
        KE_LOG_ERROR() << "JobObject: CreateJobObjectW failed, GetLastError=" << GetLastError();
        return;
    }

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(m_handle, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        KE_LOG_ERROR() << "JobObject: SetInformationJobObject failed, GetLastError=" << GetLastError();
        CloseHandle(m_handle);
        m_handle = nullptr;
    }
}

JobObject::~JobObject() {
    // Fermer ce handle est ce qui déclenche JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
    // si c'était le dernier handle référençant ce job -- exactement le
    // comportement voulu pour une fermeture normale ET pour une terminaison
    // abrupte du process propriétaire (le noyau ferme les handles restants).
    if (m_handle) {
        CloseHandle(m_handle);
    }
}

bool JobObject::isValid() const {
    return m_handle != nullptr;
}

bool JobObject::assignProcess(qint64 pid) {
    if (!m_handle) {
        return false;
    }
    HANDLE processHandle = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid));
    if (!processHandle) {
        KE_LOG_ERROR() << "JobObject::assignProcess: OpenProcess failed for pid " << pid
                        << ", GetLastError=" << GetLastError();
        return false;
    }
    const bool assigned = AssignProcessToJobObject(m_handle, processHandle) != 0;
    if (!assigned) {
        KE_LOG_ERROR() << "JobObject::assignProcess: AssignProcessToJobObject failed for pid " << pid
                        << ", GetLastError=" << GetLastError();
    }
    CloseHandle(processHandle);
    return assigned;
}

#else

JobObject::JobObject() = default;
JobObject::~JobObject() = default;
bool JobObject::isValid() const { return false; }
bool JobObject::assignProcess(qint64) { return false; }

#endif

} // namespace killcore
