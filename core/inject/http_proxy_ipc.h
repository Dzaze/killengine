#pragma once

// État partagé entre KillEngine.exe et la DLL injectée http_proxy_handler.
// Pattern POD fixe sans dépendance Qt/killcore, comme api_hook_ipc.h.
//
// Le proxy HTTP intercepte les requêtes WinINet/WinHTTP et les stocke dans
// un buffer circulaire. KillEngine lit les requêtes et peut modifier le body
// d'une requête spécifique avant qu'elle ne soit envoyée.

#include <cstdint>
#include <cstdio>
#include <cwchar>

namespace killcore {

// Nombre max de requêtes en buffer circulaire
constexpr int kHttpProxyMaxRequests = 64;
// Taille max d'un body (requête ou réponse)
constexpr int kHttpProxyMaxBodySize = 8192;
// Taille max d'une URL
constexpr int kHttpProxyMaxUrlSize = 1024;

#pragma pack(push, 1)
struct HttpProxyRequestEntry {
    wchar_t method[16];       // GET, POST, PUT, DELETE, etc.
    wchar_t url[kHttpProxyMaxUrlSize];
    wchar_t requestBody[kHttpProxyMaxBodySize];
    wchar_t responseBody[kHttpProxyMaxBodySize];
    int64_t timestamp;        // ms since epoch
    int32_t modified;         // 1 si le body a été modifié
    int32_t active;           // 1 si cette entrée est valide
    int32_t requestHandled;   // 1 si la requête a été envoyée (avec body modifié ou non)
};

struct HttpProxyIpcState {
    // Config écrite par KillEngine AVANT l'injection.
    int32_t interceptHttps;   // 1 = intercepter HTTPS aussi, 0 = HTTP seul

    // État vivant écrit par le handler.
    volatile long active;           // 1 une fois les hooks posés
    volatile long installError;     // 1 si la pose des hooks a échoué
    volatile long long totalIntercepted; // Nombre total de requêtes interceptées

    // Buffer circulaire de requêtes
    HttpProxyRequestEntry requests[kHttpProxyMaxRequests];
    volatile long writeIndex;       // Index d'écriture (handler)
    volatile long readIndex;        // Index de lecture (KillEngine)

    // Modification de requête (KillEngine → handler)
    int32_t modifyRequestIndex;     // Index de la requête à modifier (-1 = aucune)
    wchar_t modifyRequestBody[kHttpProxyMaxBodySize]; // Nouveau body
    volatile long modifyRequested;  // 1 pour demander une modification

    // Contrôle
    volatile long removeRequested;  // 1 pour retirer les hooks
};
#pragma pack(pop)

/// Nom du mapping partagé, dérivé du PID cible.
inline void buildHttpProxyMappingName(uint32_t pid, wchar_t* buffer, size_t bufferCount) {
    swprintf_s(buffer, bufferCount, L"Local\\KillEngineHttpProxy_%u", pid);
}

} // namespace killcore
