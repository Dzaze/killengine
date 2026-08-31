#pragma once

#ifdef CDP_PIPE_HELPER_EXPORTS
#define CDP_PIPE_HELPER_API __declspec(dllexport)
#else
#define CDP_PIPE_HELPER_API __declspec(dllimport)
#endif

#include <windows.h>
#include <string>
#include <functional>
#include <thread>
#include <atomic>
#include <queue>
#include <mutex>
#include <condition_variable>

// Version C++ pure pour la DLL (pas de Qt ici)

namespace killcore {

/**
 * @brief Helper injecté qui fait le pont entre WebView2 CDP et pipe nommé.
 *
 * Ce code s'exécute DANS le processus cible (Solitaire/SC2) et :
 * 1. Se connecte à WebView2 CDP via WebSocket (localhost:9333)
 * 2. Crée un pipe nommé local
 * 3. Relaie les messages dans les deux sens
 */
class CDP_PIPE_HELPER_API CdpPipeHelper {
public:
    using LogCallback = std::function<void(const char* message)>;

    CdpPipeHelper();
    ~CdpPipeHelper();

    /**
     * @brief Démarre le helper.
     * @param pipeName Nom du pipe à créer
     * @param cdpPort Port CDP WebView2 (défaut: 9333)
     * @return true si démarré avec succès
     */
    bool start(const char* pipeName, int cdpPort = 9333);

    /**
     * @brief Arrête le helper.
     */
    void stop();

    /**
     * @brief Vérifie si le helper est en cours d'exécution.
     */
    bool isRunning() const;

    /**
     * @brief Définit un callback pour les logs.
     */
    void setLogCallback(LogCallback callback);

    /**
     * @brief Récupère la dernière erreur.
     */
    const char* getLastError() const;

private:
    // Threads
    void cdpToPipeThread();
    void pipeToCdpThread();

    // WebSocket (WinHTTP)
    bool connectWebSocket(const char* host, int port);
    void disconnectWebSocket();
    bool sendWebSocket(const std::string& message);
    std::string receiveWebSocket();

    // Pipe
    bool createPipeServer(const char* pipeName);
    void closePipe();
    bool sendPipe(const std::string& message);
    std::string receivePipe();

    // WebSocket framing
    std::string wsEncode(const std::string& message);
    std::string wsDecode(const std::string& frame);

    void log(const char* fmt, ...);

    // État
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_cdpConnected{false};
    std::atomic<bool> m_pipeConnected{false};

    // Handles
    void* m_wsSocket{nullptr};  // WinHTTP WebSocket handle
    HANDLE m_pipeHandle{INVALID_HANDLE_VALUE};

    // Threads
    std::thread m_cdpThread;
    std::thread m_pipeThread;

    // Buffer de messages
    std::queue<std::string> m_cdpToPipeQueue;
    std::queue<std::string> m_pipeToCdpQueue;
    std::mutex m_cdpMutex;
    std::mutex m_pipeMutex;
    std::condition_variable m_cdpCv;
    std::condition_variable m_pipeCv;

    // Callback
    LogCallback m_logCallback;
    char m_lastError[256];
};

// ============================================================================
// Interface C pour l'injection
// ============================================================================

extern "C" {

/**
 * @brief Point d'entrée pour l'injection DLL.
 *
 * Cette fonction est appelée par l'injecteur après le chargement de la DLL.
 * Elle démarre le helper dans un thread séparé.
 *
 * @param pipeName Nom du pipe à créer (ex: "KillEngineCdpBridge_1234")
 * @param cdpPort Port CDP (défaut: 9333)
 * @return 0 si succès, code erreur sinon
 */
CDP_PIPE_HELPER_API int StartCdpPipeHelper(const char* pipeName, int cdpPort);

/**
 * @brief Arrête le helper.
 */
CDP_PIPE_HELPER_API void StopCdpPipeHelper();

/**
 * @brief Vérifie si le helper est actif.
 */
CDP_PIPE_HELPER_API int IsCdpPipeHelperRunning();

/**
 * @brief Récupère la dernière erreur.
 */
CDP_PIPE_HELPER_API const char* GetCdpPipeHelperError();

} // extern "C"

} // namespace killcore
