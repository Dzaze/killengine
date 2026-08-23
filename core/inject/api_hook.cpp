#include "api_hook.h"

#include "dll_injector.h"
#include "logging/logger.h"

#ifdef Q_OS_WIN
#include <windows.h>
#endif

#include <chrono>
#include <thread>

namespace killcore {

ApiHookSession::~ApiHookSession() {
    stop();
}

#ifdef Q_OS_WIN

namespace {

// Bornes des tableaux wchar_t de l'IPC (voir api_hook_ipc.h).
constexpr int kModuleNameMaxChars = 64;
constexpr int kFunctionNameMaxChars = 128;

} // namespace

bool ApiHookSession::start(const ProcessHandle& process, const ApiHookConfig& config,
                           const QString& handlerPath, QString* error) {
    if (m_active) {
        if (error) *error = "Une interception est déjà active sur cette session.";
        return false;
    }
    if (config.moduleName.isEmpty() || config.functionName.isEmpty()) {
        if (error) *error = "Module et fonction requis (ex: kernel32.dll!CreateFileW).";
        return false;
    }

    const uint32_t pid = process.pid();
    wchar_t mappingName[64];
    buildApiHookMappingName(pid, mappingName, 64);

    // Un mapping déjà existant pour ce PID = le handler a déjà été injecté une
    // fois (il reste chargé jusqu'à la fin du processus cible). DllMain ne se
    // rejoue pas sur un LoadLibraryW d'un module déjà chargé : impossible de
    // repartir proprement → refus explicite plutôt qu'un état silencieusement
    // faux (même position que inprocess_breakpoint.h, à l'inverse du speedhack
    // qui peut se réactiver car son état n'a rien à réinstaller).
    HANDLE existing = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, mappingName);
    if (existing) {
        CloseHandle(existing);
        if (error) *error = "Un composant d'interception a déjà été injecté dans cette cible "
                            "(une session par lancement de la cible, comme le breakpoint in-process).";
        return false;
    }

    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                        0, sizeof(ApiHookIpcState), mappingName);
    if (!mapping) {
        if (error) *error = "CreateFileMapping a échoué (IPC interception).";
        return false;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(mapping);
        if (error) *error = "Un composant d'interception a déjà été injecté dans cette cible.";
        return false;
    }

    auto* state = static_cast<ApiHookIpcState*>(
        MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(ApiHookIpcState)));
    if (!state) {
        CloseHandle(mapping);
        if (error) *error = "MapViewOfFile a échoué (IPC interception).";
        return false;
    }

    // Config initiale, écrite AVANT l'injection pour que le handler la lise
    // dès son premier accès au mapping.
    ZeroMemory(state, sizeof(ApiHookIpcState));
    // toWCharArray ne borne pas la destination : tronquer la QString AVANT
    // la copie pour ne jamais déborder du tableau wchar_t de l'IPC.
    const QString moduleName = config.moduleName.left(kModuleNameMaxChars - 1);
    const QString functionName = config.functionName.left(kFunctionNameMaxChars - 1);
    moduleName.toWCharArray(state->moduleName);
    state->moduleName[moduleName.size()] = L'\0';
    functionName.toWCharArray(state->functionName);
    state->functionName[functionName.size()] = L'\0';
    state->mode = static_cast<int32_t>(config.mode);
    state->forcedReturnValue = config.forcedReturnValue;
    state->active = 0;
    state->installError = 0;
    state->resolveError = 0;
    state->unsupportedConv = 0;
    state->callCount = 0;
    state->removeRequested = 0;

    const auto injected = killcore::injectDll(process, handlerPath);
    if (!injected.success) {
        UnmapViewOfFile(state);
        CloseHandle(mapping);
        if (error) *error = "Injection du composant d'interception échouée: " + injected.error;
        return false;
    }

    // Attend que le handler signale active/installError/resolveError (borné :
    // la résolution du module peut prendre jusqu'à ~4s dans le handler si le
    // module cible n'est chargé que tardivement).
    const auto installStart = std::chrono::steady_clock::now();
    while (!state->active && !state->installError && !state->resolveError) {
        if (std::chrono::steady_clock::now() - installStart > std::chrono::milliseconds(6000)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    if (state->resolveError) {
        UnmapViewOfFile(state);
        CloseHandle(mapping);
        if (error) *error = QStringLiteral("%1!%2 introuvable dans la cible (module non chargé ou fonction non exportée).")
                                 .arg(config.moduleName, config.functionName);
        return false;
    }
    if (state->installError || !state->active) {
        UnmapViewOfFile(state);
        CloseHandle(mapping);
        if (error) *error = state->installError
            ? QStringLiteral("La pose du hook MinHook a échoué dans la cible.")
            : QStringLiteral("Timeout: le composant d'interception ne s'est pas installé.");
        return false;
    }

    m_mapping = mapping;
    m_state = state;
    m_pid = pid;
    m_active = true;
    KE_LOG_INFO() << "ApiHook: hook installé (pid=" << pid << ", " << config.moduleName.toStdString()
                  << "!" << config.functionName.toStdString() << ", mode=" << static_cast<int>(config.mode) << ")";
    return true;
}

void ApiHookSession::stop() {
    if (!m_active) {
        return;
    }
    if (m_state) {
        // Demande le retrait du hook au handler, puis attend sa confirmation
        // (active retombe à 0). Borné : si le handler est mort, on ne bloque pas
        // KillEngine — la cible garde un hook résiduel jusqu'à sa fermeture,
        // mais le mode Count appelle l'original donc l'impact fonctionnel est nul.
        m_state->removeRequested = 1;
        const auto stopStart = std::chrono::steady_clock::now();
        while (m_state->active) {
            if (std::chrono::steady_clock::now() - stopStart > std::chrono::milliseconds(3000)) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        UnmapViewOfFile(m_state);
    }
    if (m_mapping) {
        CloseHandle(static_cast<HANDLE>(m_mapping));
    }
    KE_LOG_INFO() << "ApiHook: session arrêtée (pid=" << m_pid << ")";
    m_mapping = nullptr;
    m_state = nullptr;
    m_pid = 0;
    m_active = false;
}

ApiHookStats ApiHookSession::stats() const {
    ApiHookStats stats;
    if (!m_state) {
        return stats;
    }
    stats.active = m_state->active != 0;
    stats.installError = m_state->installError != 0;
    stats.resolveError = m_state->resolveError != 0;
    stats.unsupportedConv = m_state->unsupportedConv != 0;
    stats.callCount = static_cast<uint64_t>(m_state->callCount);
    return stats;
}

#else

bool ApiHookSession::start(const ProcessHandle&, const ApiHookConfig&, const QString&, QString* error) {
    if (error) *error = "API hooking is Windows-only";
    return false;
}

void ApiHookSession::stop() {}

ApiHookStats ApiHookSession::stats() const {
    return {};
}

#endif

} // namespace killcore