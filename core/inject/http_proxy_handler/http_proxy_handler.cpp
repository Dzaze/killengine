// KillEngineHttpProxyHandler.dll — handler injecté dans le processus cible pour
// intercepter les requêtes HTTP/HTTPS via MinHook sur WinINet/WinHTTP.
//
// Volontairement indépendant de Qt/killcore (même raisonnement documenté dans
// api_hook_handler.cpp) : cette DLL est chargée dans un processus tiers.
//
// Principe : KillEngine écrit la config (interceptHttps) dans le mapping partagé
// AVANT l'injection. Une fois chargée, la DLL :
//   1. Ouvre le mapping partagé (nom dérivé de son propre PID).
//   2. Résout HttpSendRequest (wininet.dll) et WinHttpSendRequest (winhttp.dll).
//   3. Pose des hooks MinHook qui capturent method, URL, body.
//   4. Si modifyRequested, remplace le body avant d'appeler l'original.
//   5. Surveille removeRequested pour retirer les hooks.

#include "../http_proxy_ipc.h"

#include <windows.h>
#include <winhttp.h>
#include <wininet.h>
#include <cstdlib>

#include "MinHook.h"

namespace {

killcore::HttpProxyIpcState* g_state = nullptr;
HANDLE g_mapping = nullptr;

// WinINet
typedef BOOL (WINAPI* HttpSendRequestFn)(HINTERNET hRequest, LPCWSTR lpszHeaders,
                                          DWORD dwHeadersLength, LPVOID lpOptional,
                                          DWORD dwOptionalLength);
typedef BOOL (WINAPI* HttpSendRequestAFn)(HINTERNET hRequest, LPCSTR lpszHeaders,
                                           DWORD dwHeadersLength, LPVOID lpOptional,
                                           DWORD dwOptionalLength);

// WinHTTP
typedef BOOL (WINAPI* WinHttpSendRequestFn)(HINTERNET hRequest, LPCWSTR lpszHeaders,
                                             DWORD dwHeadersLength, LPVOID lpOptional,
                                             DWORD dwOptionalLength,
                                             DWORD dwTotalLength, DWORD_PTR dwContext);

HttpSendRequestFn g_originalHttpSendRequest = nullptr;
HttpSendRequestAFn g_originalHttpSendRequestA = nullptr;
WinHttpSendRequestFn g_originalWinHttpSendRequest = nullptr;

int GetNextWriteIndex() {
    long idx = g_state->writeIndex;
    long next = (idx + 1) % killcore::kHttpProxyMaxRequests;
    // Si on rattrape le readIndex, on écrase l'ancienne entrée
    return static_cast<int>(next);
}

void CaptureRequest(const wchar_t* method, const wchar_t* url,
                    LPVOID lpOptional, DWORD dwOptionalLength) {
    if (!g_state) return;

    int idx = GetNextWriteIndex();
    auto& entry = g_state->requests[idx];

    // Vérifier si l'entrée a déjà été lue
    long readIdx = g_state->readIndex;
    if (idx == readIdx && entry.active) {
        // Le lecteur n'a pas encore lu cette entrée, on l'écrase quand même
    }

    ZeroMemory(&entry, sizeof(entry));

    // Method
    if (method) {
        wcsncpy_s(entry.method, _countof(entry.method), method, _TRUNCATE);
    }

    // URL
    if (url) {
        wcsncpy_s(entry.url, _countof(entry.url), url, _TRUNCATE);
    }

    // Request body
    if (lpOptional && dwOptionalLength > 0 && dwOptionalLength < killcore::kHttpProxyMaxBodySize) {
        // Copier en wchar_t (best-effort, assume UTF-16 ou ASCII)
        memcpy(entry.requestBody, lpOptional, dwOptionalLength);
        entry.requestBody[dwOptionalLength / sizeof(wchar_t)] = L'\0';
    }

    entry.timestamp = GetTickCount64();
    entry.modified = 0;
    entry.active = 1;
    entry.requestHandled = 0;

    g_state->writeIndex = idx;
    InterlockedIncrement64(&g_state->totalIntercepted);
}

BOOL WINAPI HttpSendRequestDetour(HINTERNET hRequest, LPCWSTR lpszHeaders,
                                   DWORD dwHeadersLength, LPVOID lpOptional,
                                   DWORD dwOptionalLength) {
    if (g_state) {
        CaptureRequest(L"POST", L"(WinINet)", lpOptional, dwOptionalLength);

        // Vérifier si une modification est demandée pour cette requête
        long modifyIdx = g_state->modifyRequestIndex;
        if (modifyIdx >= 0 && modifyIdx < killcore::kHttpProxyMaxRequests) {
            auto& modEntry = g_state->requests[modifyIdx];
            if (modEntry.active && !modEntry.requestHandled && g_state->modifyRequested) {
                // Remplacer le body
                size_t newLen = wcslen(g_state->modifyRequestBody);
                if (newLen > 0 && newLen * sizeof(wchar_t) < killcore::kHttpProxyMaxBodySize) {
                    lpOptional = g_state->modifyRequestBody;
                    dwOptionalLength = static_cast<DWORD>(newLen * sizeof(wchar_t));
                    modEntry.modified = 1;
                }
                modEntry.requestHandled = 1;
                g_state->modifyRequested = 0;
                g_state->modifyRequestIndex = -1;
            }
        }
    }
    return g_originalHttpSendRequest(hRequest, lpszHeaders, dwHeadersLength, lpOptional, dwOptionalLength);
}

BOOL WINAPI HttpSendRequestADetour(HINTERNET hRequest, LPCSTR lpszHeaders,
                                    DWORD dwHeadersLength, LPVOID lpOptional,
                                    DWORD dwOptionalLength) {
    if (g_state) {
        CaptureRequest(L"POST", L"(WinINet-A)", lpOptional, dwOptionalLength);

        long modifyIdx = g_state->modifyRequestIndex;
        if (modifyIdx >= 0 && modifyIdx < killcore::kHttpProxyMaxRequests) {
            auto& modEntry = g_state->requests[modifyIdx];
            if (modEntry.active && !modEntry.requestHandled && g_state->modifyRequested) {
                size_t newLen = wcslen(g_state->modifyRequestBody);
                if (newLen > 0 && newLen * sizeof(wchar_t) < killcore::kHttpProxyMaxBodySize) {
                    // Convertir wchar_t → char (best-effort, ASCII)
                    static char asciiBuf[killcore::kHttpProxyMaxBodySize];
                    size_t converted = 0;
                    wcstombs_s(&converted, asciiBuf, sizeof(asciiBuf), g_state->modifyRequestBody, _TRUNCATE);
                    lpOptional = asciiBuf;
                    dwOptionalLength = static_cast<DWORD>(converted - 1);
                    modEntry.modified = 1;
                }
                modEntry.requestHandled = 1;
                g_state->modifyRequested = 0;
                g_state->modifyRequestIndex = -1;
            }
        }
    }
    return g_originalHttpSendRequestA(hRequest, lpszHeaders, dwHeadersLength, lpOptional, dwOptionalLength);
}

BOOL WINAPI WinHttpSendRequestDetour(HINTERNET hRequest, LPCWSTR lpszHeaders,
                                      DWORD dwHeadersLength, LPVOID lpOptional,
                                      DWORD dwOptionalLength, DWORD dwTotalLength,
                                      DWORD_PTR dwContext) {
    if (g_state) {
        CaptureRequest(L"POST", L"(WinHTTP)", lpOptional, dwOptionalLength);

        long modifyIdx = g_state->modifyRequestIndex;
        if (modifyIdx >= 0 && modifyIdx < killcore::kHttpProxyMaxRequests) {
            auto& modEntry = g_state->requests[modifyIdx];
            if (modEntry.active && !modEntry.requestHandled && g_state->modifyRequested) {
                size_t newLen = wcslen(g_state->modifyRequestBody);
                if (newLen > 0 && newLen * sizeof(wchar_t) < killcore::kHttpProxyMaxBodySize) {
                    lpOptional = g_state->modifyRequestBody;
                    dwOptionalLength = static_cast<DWORD>(newLen * sizeof(wchar_t));
                    modEntry.modified = 1;
                }
                modEntry.requestHandled = 1;
                g_state->modifyRequested = 0;
                g_state->modifyRequestIndex = -1;
            }
        }
    }
    return g_originalWinHttpSendRequest(hRequest, lpszHeaders, dwHeadersLength, lpOptional,
                                         dwOptionalLength, dwTotalLength, dwContext);
}

void CloseMapping() {
    if (g_state) {
        UnmapViewOfFile(g_state);
        g_state = nullptr;
    }
    if (g_mapping) {
        CloseHandle(g_mapping);
        g_mapping = nullptr;
    }
}

DWORD WINAPI InstallThread(LPVOID) {
    wchar_t name[64];
    killcore::buildHttpProxyMappingName(GetCurrentProcessId(), name, 64);

    g_mapping = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, name);
    if (!g_mapping) {
        return 1;
    }
    g_state = static_cast<killcore::HttpProxyIpcState*>(
        MapViewOfFile(g_mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(killcore::HttpProxyIpcState)));
    if (!g_state) {
        CloseHandle(g_mapping);
        return 1;
    }

    // Initialiser le state
    g_state->writeIndex = 0;
    g_state->readIndex = 0;
    g_state->modifyRequestIndex = -1;
    g_state->modifyRequested = 0;
    g_state->active = 0;
    g_state->installError = 0;
    g_state->totalIntercepted = 0;

    if (MH_Initialize() != MH_OK) {
        g_state->installError = 1;
        CloseMapping();
        return 1;
    }

    int hooksInstalled = 0;

    // WinINet: HttpSendRequestW
    HMODULE wininet = GetModuleHandleW(L"wininet.dll");
    if (wininet) {
        void* target = GetProcAddress(wininet, "HttpSendRequestW");
        if (target && MH_CreateHook(target, reinterpret_cast<void*>(&HttpSendRequestDetour),
                                     reinterpret_cast<LPVOID*>(&g_originalHttpSendRequest)) == MH_OK) {
            if (MH_EnableHook(target) == MH_OK) hooksInstalled++;
        }
        void* targetA = GetProcAddress(wininet, "HttpSendRequestA");
        if (targetA && MH_CreateHook(targetA, reinterpret_cast<void*>(&HttpSendRequestADetour),
                                      reinterpret_cast<LPVOID*>(&g_originalHttpSendRequestA)) == MH_OK) {
            if (MH_EnableHook(targetA) == MH_OK) hooksInstalled++;
        }
    }

    // WinHTTP: WinHttpSendRequest
    HMODULE winhttp = GetModuleHandleW(L"winhttp.dll");
    if (winhttp) {
        void* target = GetProcAddress(winhttp, "WinHttpSendRequest");
        if (target && MH_CreateHook(target, reinterpret_cast<void*>(&WinHttpSendRequestDetour),
                                     reinterpret_cast<LPVOID*>(&g_originalWinHttpSendRequest)) == MH_OK) {
            if (MH_EnableHook(target) == MH_OK) hooksInstalled++;
        }
    }

    if (hooksInstalled == 0) {
        MH_Uninitialize();
        g_state->installError = 1;
        CloseMapping();
        return 1;
    }

    InterlockedExchange(&g_state->active, 1);

    // Boucle de surveillance
    while (InterlockedExchange(&g_state->removeRequested, 0) == 0) {
        Sleep(100);
    }

    // Retirer tous les hooks
    if (g_originalHttpSendRequest) MH_RemoveHook(g_originalHttpSendRequest);
    if (g_originalHttpSendRequestA) MH_RemoveHook(g_originalHttpSendRequestA);
    if (g_originalWinHttpSendRequest) MH_RemoveHook(g_originalWinHttpSendRequest);
    MH_Uninitialize();
    InterlockedExchange(&g_state->active, 0);

    return 0;
}

} // namespace

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        CreateThread(nullptr, 0, InstallThread, nullptr, 0, nullptr);
    }
    return TRUE;
}
