#pragma once

#include "process/process_handle.h"
#include "inject/http_proxy_ipc.h"

#include <QString>
#include <QVector>

#include <cstdint>

namespace killcore {

struct HttpProxyRequest {
    QString method;
    QString url;
    QString requestBody;
    QString responseBody;
    qint64 timestamp;
    bool modified{false};
};

struct HttpProxyStats {
    bool active{false};
    bool installError{false};
    long long totalIntercepted{0};
};

/**
 * @brief Intercepte les requêtes HTTP/HTTPS du processus cible via injection
 * DLL + MinHook sur HttpSendRequest/WinHttpSendRequest.
 *
 * Le handler injecté capture method, URL, body de chaque requête et les
 * stocke dans un buffer circulaire IPC. KillEngine peut modifier le body
 * d'une requête avant qu'elle ne soit envoyée.
 */
class HttpProxySession {
public:
    HttpProxySession() = default;
    ~HttpProxySession();

    HttpProxySession(const HttpProxySession&) = delete;
    HttpProxySession& operator=(const HttpProxySession&) = delete;

    /// Injecte le handler et pose les hooks sur HttpSendRequest/WinHttpSendRequest.
    bool start(const ProcessHandle& process, bool interceptHttps,
               const QString& handlerPath, QString* error);

    /// Demande au handler de retirer les hooks.
    void stop();

    /// Lit les requêtes interceptées depuis le buffer IPC.
    QVector<HttpProxyRequest> getRequests() const;

    /// Modifie le body d'une requête interceptée (par index).
    bool modifyRequest(int index, const QString& newBody);

    HttpProxyStats stats() const;
    bool isActive() const { return m_active; }

private:
    void* m_mapping{nullptr};
    HttpProxyIpcState* m_state{nullptr};
    uint32_t m_pid{0};
    bool m_active{false};
};

} // namespace killcore
