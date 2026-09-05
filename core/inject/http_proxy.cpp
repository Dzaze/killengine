#include "http_proxy.h"

#include "dll_injector.h"
#include "logging/logger.h"

#ifdef Q_OS_WIN
#include <windows.h>
#endif

#include <chrono>
#include <thread>

namespace killcore {

HttpProxySession::~HttpProxySession() {
    stop();
}

#ifdef Q_OS_WIN

bool HttpProxySession::start(const ProcessHandle& process, bool interceptHttps,
                             const QString& handlerPath, QString* error) {
    if (m_active) {
        if (error) *error = "Un proxy HTTP est déjà actif sur cette session.";
        return false;
    }
    if (handlerPath.isEmpty()) {
        if (error) *error = "Chemin du handler requis.";
        return false;
    }

    const uint32_t pid = process.pid();
    wchar_t mappingName[64];
    buildHttpProxyMappingName(pid, mappingName, 64);

    // Un mapping déjà existant = handler déjà injecté
    HANDLE existing = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, mappingName);
    if (existing) {
        CloseHandle(existing);
        if (error) *error = "Un proxy HTTP a déjà été injecté dans cette cible "
                            "(une session par lancement de la cible).";
        return false;
    }

    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                        0, sizeof(HttpProxyIpcState), mappingName);
    if (!mapping) {
        if (error) *error = "CreateFileMapping a échoué (IPC proxy HTTP).";
        return false;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(mapping);
        if (error) *error = "Un proxy HTTP a déjà été injecté dans cette cible.";
        return false;
    }

    auto* state = static_cast<HttpProxyIpcState*>(
        MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(HttpProxyIpcState)));
    if (!state) {
        CloseHandle(mapping);
        if (error) *error = "MapViewOfFile a échoué (IPC proxy HTTP).";
        return false;
    }

    // Config initiale
    ZeroMemory(state, sizeof(HttpProxyIpcState));
    state->interceptHttps = interceptHttps ? 1 : 0;
    state->modifyRequestIndex = -1;
    state->active = 0;
    state->installError = 0;
    state->totalIntercepted = 0;
    state->removeRequested = 0;

    const auto injected = killcore::injectDll(process, handlerPath);
    if (!injected.success) {
        UnmapViewOfFile(state);
        CloseHandle(mapping);
        if (error) *error = "Injection du handler proxy HTTP échouée: " + injected.error;
        return false;
    }

    // Attend que le handler signale active/installError
    const auto installStart = std::chrono::steady_clock::now();
    while (!state->active && !state->installError) {
        if (std::chrono::steady_clock::now() - installStart > std::chrono::milliseconds(6000)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    if (state->installError) {
        UnmapViewOfFile(state);
        CloseHandle(mapping);
        if (error) *error = "Le handler proxy HTTP n'a pas pu poser les hooks (wininet/winhttp introuvables ou MinHook échoué).";
        return false;
    }

    m_mapping = mapping;
    m_state = state;
    m_pid = pid;
    m_active = true;

    KE_LOG_INFO() << "HttpProxySession: started pid=" << pid << " interceptHttps=" << interceptHttps;
    return true;
}

void HttpProxySession::stop() {
    if (!m_active || !m_state) return;

    m_state->removeRequested = 1;

    // Attend que le handler confirme
    const auto removeStart = std::chrono::steady_clock::now();
    while (m_state->active) {
        if (std::chrono::steady_clock::now() - removeStart > std::chrono::milliseconds(3000)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    UnmapViewOfFile(m_state);
    CloseHandle(m_mapping);
    m_mapping = nullptr;
    m_state = nullptr;
    m_active = false;

    KE_LOG_INFO() << "HttpProxySession: stopped pid=" << m_pid;
}

QVector<HttpProxyRequest> HttpProxySession::getRequests() const {
    QVector<HttpProxyRequest> result;
    if (!m_state) return result;

    long readIdx = m_state->readIndex;
    long writeIdx = m_state->writeIndex;

    // Lire toutes les entrées non lues
    while (readIdx != writeIdx) {
        const auto& entry = m_state->requests[readIdx];
        if (entry.active) {
            HttpProxyRequest req;
            req.method = QString::fromWCharArray(entry.method);
            req.url = QString::fromWCharArray(entry.url);
            req.requestBody = QString::fromWCharArray(entry.requestBody);
            req.responseBody = QString::fromWCharArray(entry.responseBody);
            req.timestamp = entry.timestamp;
            req.modified = entry.modified != 0;
            result.append(req);
        }
        readIdx = (readIdx + 1) % kHttpProxyMaxRequests;
    }

    return result;
}

bool HttpProxySession::modifyRequest(int index, const QString& newBody) {
    if (!m_state || index < 0 || index >= kHttpProxyMaxRequests) return false;

    auto& entry = m_state->requests[index];
    if (!entry.active || entry.requestHandled) return false;

    // Copier le nouveau body
    const int maxChars = kHttpProxyMaxBodySize - 1;
    const QString truncated = newBody.left(maxChars);
    truncated.toWCharArray(const_cast<wchar_t*>(m_state->modifyRequestBody));
    m_state->modifyRequestBody[truncated.size()] = L'\0';

    m_state->modifyRequestIndex = index;
    m_state->modifyRequested = 1;

    return true;
}

HttpProxyStats HttpProxySession::stats() const {
    HttpProxyStats s;
    if (m_state) {
        s.active = m_state->active && !m_state->removeRequested;
        s.installError = m_state->installError != 0;
        s.totalIntercepted = m_state->totalIntercepted;
    }
    return s;
}

#else // non-Windows

bool HttpProxySession::start(const ProcessHandle&, bool, const QString&, QString* error) {
    if (error) *error = "Fonctionnalité Windows uniquement.";
    return false;
}

void HttpProxySession::stop() {}
QVector<HttpProxyRequest> HttpProxySession::getRequests() const { return {}; }
bool HttpProxySession::modifyRequest(int, const QString&) { return false; }
HttpProxyStats HttpProxySession::stats() const { return {}; }

#endif

} // namespace killcore
