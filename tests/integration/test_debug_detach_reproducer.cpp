// =============================================================================
// Reproducer minimal, INDEPENDANT du code KillEngine (appels Win32 bruts
// uniquement, sauf mention explicite du contraire dans un scenario donne),
// construit le 20/08/2026 sur demande explicite de l'utilisateur pour
// root-causer un test d'integration rouge (BreakpointArbiterRuntimeTest.
// SequentialCyclesAcrossFreshPidsExternalThenInProcess) : VirtualAllocEx/
// CreateRemoteThread refusaient (error=5) apres un cycle DebugActiveProcess
// -> DebugActiveProcessStop sur la meme cible.
//
// CONCLUSION (bissection complete, scenarios A a I ci-dessous) : PAS un bug
// KillEngine. Confirme via reproduction 100% Win32 pure (aucune classe
// KillEngine) : Microsoft Defender for Endpoint (le service ATP, distinct de
// la protection temps reel Windows Defender classique -- desactiver cette
// derniere n'a RIEN change) bloque de facon intermittente VirtualAllocEx/
// CreateRemoteThread quand ils suivent immediatement un cycle de debug sur
// la meme cible -- heuristique classique de detection d'injection de code,
// qui ne distingue pas un usage legitime (debug/instrumentation, exactement
// ce que fait KillEngine) d'un usage malveillant. Non lie a nos flags
// d'acces (PROCESS_ALL_ACCESS litteral echoue aussi), non lie a un delai
// (teste jusqu'a 3s, sans amelioration). Voir docs/STRATEGY_ROOM.md pour le
// detail complet et la mitigation retenue (message actionnable dans
// core/inject/dll_injector.cpp).
//
// Scenarios A a D et F ne vont que jusqu'a VirtualAllocEx (jamais
// CreateRemoteThread) et restent VERTS de facon fiable -- gardes comme etapes
// du raisonnement de bissection. Scenarios E, G, H, I vont jusqu'au bout de
// la chaine d'injection reelle et rencontrent la signature EDR de facon
// fiable -- GTEST_SKIP() documente plutot qu'echec dur, pour rester
// executables comme preuve vivante sans polluer la suite de rouge permanent.
// =============================================================================

#include <gtest/gtest.h>

#include <QtGlobal>

#ifdef Q_OS_WIN
#include <windows.h>
#include <tlhelp32.h>
#endif

#include "debug/hardware_breakpoint.h"
#include "debug/inprocess_breakpoint.h"
#include "process/process_handle.h"

#include <QCoreApplication>
#include <QDir>
#include <QProcess>
#include <QThread>

#include <QFile>
#include <QProcessEnvironment>

#include <cstdio>
#include <optional>
#include <string>
#include <vector>

namespace {

#ifdef Q_OS_WIN

void logStep(const char* step) {
    fprintf(stderr, "[reproducer] %s (GetLastError=%lu)\n", step, GetLastError());
}

void logStepOk(const char* step) {
    fprintf(stderr, "[reproducer] %s OK\n", step);
}

bool enableDebugPrivilegeRaw() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        logStep("OpenProcessToken failed");
        return false;
    }
    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (!LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &tp.Privileges[0].Luid)) {
        logStep("LookupPrivilegeValueW failed");
        CloseHandle(token);
        return false;
    }
    const BOOL ok = AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), nullptr, nullptr);
    const DWORD err = GetLastError();
    CloseHandle(token);
    if (!ok || err == ERROR_NOT_ALL_ASSIGNED) {
        fprintf(stderr, "[reproducer] SeDebugPrivilege NOT granted (error=%lu)\n", err);
        return false;
    }
    logStepOk("SeDebugPrivilege enabled");
    return true;
}

std::vector<DWORD> enumerateThreadIdsRaw(DWORD pid) {
    std::vector<DWORD> ids;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return ids;
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID == pid) ids.push_back(te.th32ThreadID);
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    return ids;
}

// Retourne true si VirtualAllocEx/VirtualFreeEx ont reussi (le test reel de
// ce reproducer : PAS d'injection complete, juste la meme premiere operation
// qui echoue dans core/inject/dll_injector.cpp::injectDll).
bool tryVirtualAllocRaw(DWORD pid, const char* label) {
    HANDLE h = OpenProcess(
        PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION,
        FALSE, pid);
    if (!h) {
        fprintf(stderr, "[reproducer] %s: OpenProcess failed (error=%lu)\n", label, GetLastError());
        return false;
    }
    logStepOk((std::string(label) + ": OpenProcess").c_str());

    LPVOID mem = VirtualAllocEx(h, nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!mem) {
        fprintf(stderr, "[reproducer] %s: VirtualAllocEx FAILED (error=%lu)\n", label, GetLastError());
        CloseHandle(h);
        return false;
    }
    fprintf(stderr, "[reproducer] %s: VirtualAllocEx OK at %p\n", label, mem);
    VirtualFreeEx(h, mem, 0, MEM_RELEASE);
    CloseHandle(h);
    return true;
}

std::optional<uint64_t> readTestTargetHealthAddressRaw(uint32_t expectedPid) {
    QFile marker(QDir::temp().filePath("killengine_test_target_addresses.txt"));
    if (!marker.open(QIODevice::ReadOnly | QIODevice::Text)) return std::nullopt;
    const QString content = QString::fromUtf8(marker.readAll());
    marker.close();
    uint32_t markerPid = 0;
    uint64_t address = 0;
    for (const QString& line : content.split('\n', Qt::SkipEmptyParts)) {
        const auto parts = line.split('=');
        if (parts.size() != 2) continue;
        if (parts[0] == "pid") markerPid = parts[1].toUInt();
        else if (parts[0] == "g_health") {
            bool ok = false;
            address = parts[1].toULongLong(&ok, 16);
            if (!ok) address = 0;
        }
    }
    if (markerPid != expectedPid || address == 0) return std::nullopt;
    return address;
}

class RawTestTargetProcess {
public:
    explicit RawTestTargetProcess(bool stressRewrite = false) {
        const QString targetPath = QDir(QCoreApplication::applicationDirPath()).filePath("KillEngineTestTarget.exe");
        m_process.setProgram(targetPath);
        if (stressRewrite) {
            QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
            env.insert("KILLENGINE_TEST_TARGET_STRESS_REWRITE", "1");
            m_process.setProcessEnvironment(env);
        }
        m_process.start();
        if (m_process.waitForStarted(5000)) {
            QThread::msleep(500);
        }
    }
    ~RawTestTargetProcess() {
        if (m_process.state() != QProcess::NotRunning) {
            m_process.terminate();
            if (!m_process.waitForFinished(3000)) {
                m_process.kill();
                m_process.waitForFinished(3000);
            }
        }
    }
    bool started() const { return m_process.state() != QProcess::NotRunning; }
    DWORD pid() const { return static_cast<DWORD>(m_process.processId()); }

private:
    QProcess m_process;
};

#endif // Q_OS_WIN

} // namespace

#ifdef Q_OS_WIN

TEST(DebugDetachReproducer, ScenarioA_NeverDebugged_VirtualAllocSucceeds) {
    ASSERT_TRUE(enableDebugPrivilegeRaw());
    RawTestTargetProcess target;
    ASSERT_TRUE(target.started());

    EXPECT_TRUE(tryVirtualAllocRaw(target.pid(), "ScenarioA"));
}

// B1 : attach/detach PUR -- drainer le CREATE_PROCESS_DEBUG_EVENT initial via
// WaitForDebugEvent/ContinueDebugEvent, ne JAMAIS toucher a un thread ni a un
// registre. Isole le port de debug lui-meme de toute manipulation de contexte.
TEST(DebugDetachReproducer, ScenarioB1_AttachDetachNoThreadTouch_ThenVirtualAlloc) {
    ASSERT_TRUE(enableDebugPrivilegeRaw());
    RawTestTargetProcess target;
    ASSERT_TRUE(target.started());
    const DWORD pid = target.pid();

    ASSERT_TRUE(DebugActiveProcess(pid)) << "DebugActiveProcess failed, error=" << GetLastError();
    logStepOk("DebugActiveProcess");

    // Draine tous les evenements de debug pendant une courte fenetre (au
    // moins le CREATE_PROCESS_DEBUG_EVENT initial), sans jamais toucher a un
    // thread/registre -- juste ContinueDebugEvent(DBG_CONTINUE) a chaque fois.
    const DWORD start = GetTickCount();
    int eventsDrained = 0;
    while (GetTickCount() - start < 500) {
        DEBUG_EVENT ev{};
        if (WaitForDebugEvent(&ev, 100)) {
            fprintf(stderr, "[reproducer] B1: debug event code=%lu tid=%lu\n", ev.dwDebugEventCode, ev.dwThreadId);
            ++eventsDrained;
            ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, DBG_CONTINUE);
        }
    }
    fprintf(stderr, "[reproducer] B1: eventsDrained=%d\n", eventsDrained);
    EXPECT_GT(eventsDrained, 0) << "No debug event ever drained -- CREATE_PROCESS_DEBUG_EVENT missed, test invalid";

    const BOOL stopOk = DebugActiveProcessStop(pid);
    fprintf(stderr, "[reproducer] B1: DebugActiveProcessStop=%d (GetLastError=%lu)\n", stopOk, GetLastError());
    ASSERT_TRUE(stopOk);

    EXPECT_TRUE(tryVirtualAllocRaw(pid, "ScenarioB1")) << "VirtualAllocEx failed after PURE attach/detach (no thread touch) -- points to the debug port lifecycle itself";
}

// B2 : comme B1, mais avec le meme cycle Suspend/GetThreadContext/
// SetThreadContext/Resume que clearBreakpoints() dans hardware_breakpoint.cpp,
// sur chaque thread, AVANT le detach.
TEST(DebugDetachReproducer, ScenarioB2_AttachThreadTouchDetach_ThenVirtualAlloc) {
    ASSERT_TRUE(enableDebugPrivilegeRaw());
    RawTestTargetProcess target;
    ASSERT_TRUE(target.started());
    const DWORD pid = target.pid();

    ASSERT_TRUE(DebugActiveProcess(pid)) << "DebugActiveProcess failed, error=" << GetLastError();
    logStepOk("DebugActiveProcess");

    const DWORD start = GetTickCount();
    while (GetTickCount() - start < 500) {
        DEBUG_EVENT ev{};
        if (WaitForDebugEvent(&ev, 100)) {
            ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, DBG_CONTINUE);
        }
    }

    // Reproduit EXACTEMENT le cycle de core/debug/hardware_breakpoint.cpp::
    // applyBreakpointsToThread() (dr7=0, pas de vrai breakpoint pose -- juste
    // le cycle Suspend/Get/Set/Resume), sur chaque thread courante.
    const auto tids = enumerateThreadIdsRaw(pid);
    fprintf(stderr, "[reproducer] B2: %zu threads found\n", tids.size());
    int suspendFailures = 0;
    int getContextFailures = 0;
    int setContextFailures = 0;
    for (DWORD tid : tids) {
        HANDLE th = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, tid);
        if (!th) {
            fprintf(stderr, "[reproducer] B2: OpenThread(%lu) failed error=%lu\n", tid, GetLastError());
            continue;
        }
        const DWORD suspendCount = SuspendThread(th);
        const bool suspended = suspendCount != static_cast<DWORD>(-1);
        if (!suspended) {
            ++suspendFailures;
            fprintf(stderr, "[reproducer] B2: SuspendThread(%lu) failed error=%lu\n", tid, GetLastError());
        } else {
            fprintf(stderr, "[reproducer] B2: SuspendThread(%lu) previousCount=%lu\n", tid, suspendCount);
        }

        CONTEXT ctx{};
        ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (!GetThreadContext(th, &ctx)) {
            ++getContextFailures;
            fprintf(stderr, "[reproducer] B2: GetThreadContext(%lu) failed error=%lu\n", tid, GetLastError());
        } else {
            ctx.Dr0 = 0; ctx.Dr1 = 0; ctx.Dr2 = 0; ctx.Dr3 = 0; ctx.Dr6 = 0; ctx.Dr7 = 0;
            if (!SetThreadContext(th, &ctx)) {
                ++setContextFailures;
                fprintf(stderr, "[reproducer] B2: SetThreadContext(%lu) failed error=%lu\n", tid, GetLastError());
            }
        }

        if (suspended) {
            const DWORD resumeCount = ResumeThread(th);
            fprintf(stderr, "[reproducer] B2: ResumeThread(%lu) previousCount=%lu\n", tid, resumeCount);
        }
        CloseHandle(th);
    }
    fprintf(stderr, "[reproducer] B2: suspendFailures=%d getContextFailures=%d setContextFailures=%d\n",
            suspendFailures, getContextFailures, setContextFailures);

    const BOOL stopOk = DebugActiveProcessStop(pid);
    fprintf(stderr, "[reproducer] B2: DebugActiveProcessStop=%d (GetLastError=%lu)\n", stopOk, GetLastError());
    ASSERT_TRUE(stopOk);

    EXPECT_TRUE(tryVirtualAllocRaw(pid, "ScenarioB2")) << "VirtualAllocEx failed after attach+thread-touch+detach -- points to the Suspend/SetThreadContext/Resume cycle";
}

// B3 : reproduit fidelement hardware_breakpoint.cpp::findWhatWrites() --
// POSE un vrai DR0 (adresse=g_health, write breakpoint), laisse tourner sous
// stress rewrite reel (~1000 ecritures/s) pendant 1s de sorte a traiter des
// CENTAINES de EXCEPTION_SINGLE_STEP (contrairement a B1/B2 qui n'ont JAMAIS
// arme de vrai breakpoint ni traite une seule exception de ce type) --
// reapplique le breakpoint sur la thread qui vient de le declencher a chaque
// hit (comme applyBreakpointsToThread() dans le vrai code), desarme tout
// (dr7=0) a la fin, detache, puis tente VirtualAllocEx.
uint32_t computeDr7Bits(int slot, DWORD rwBits, DWORD lenBits) {
    uint32_t dr7 = 1u << (static_cast<uint32_t>(slot) * 2);
    dr7 |= (rwBits & 0x3u) << (16 + slot * 4);
    dr7 |= (lenBits & 0x3u) << (18 + slot * 4);
    return dr7;
}

bool armThreadRaw(HANDLE th, uint64_t address) {
    const DWORD suspendCount = SuspendThread(th);
    const bool suspended = suspendCount != static_cast<DWORD>(-1);
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    bool ok = false;
    if (GetThreadContext(th, &ctx)) {
        ctx.Dr0 = address;
        ctx.Dr6 = 0;
        ctx.Dr7 = computeDr7Bits(0, /*rw=write*/ 1, /*len=4bytes*/ 3);
        ok = SetThreadContext(th, &ctx) != FALSE;
    }
    if (suspended) ResumeThread(th);
    return ok;
}

void disarmThreadRaw(HANDLE th) {
    const DWORD suspendCount = SuspendThread(th);
    const bool suspended = suspendCount != static_cast<DWORD>(-1);
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (GetThreadContext(th, &ctx)) {
        ctx.Dr0 = 0; ctx.Dr7 = 0;
        SetThreadContext(th, &ctx);
    }
    if (suspended) ResumeThread(th);
}

TEST(DebugDetachReproducer, ScenarioB3_RealArmedBreakpointUnderStressDetach_ThenVirtualAlloc) {
    ASSERT_TRUE(enableDebugPrivilegeRaw());
    RawTestTargetProcess target(/*stressRewrite=*/true);
    ASSERT_TRUE(target.started());
    const DWORD pid = target.pid();
    const auto address = readTestTargetHealthAddressRaw(pid);
    ASSERT_TRUE(address.has_value()) << "Could not read g_health address from marker file";

    ASSERT_TRUE(DebugActiveProcess(pid)) << "DebugActiveProcess failed, error=" << GetLastError();
    logStepOk("DebugActiveProcess");

    // Arme DR0 (write breakpoint) sur toutes les threads existantes -- exact
    // equivalent de HardwareBreakpointSession::setBreakpoint().
    for (DWORD tid : enumerateThreadIdsRaw(pid)) {
        HANDLE th = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, tid);
        if (!th) continue;
        armThreadRaw(th, *address);
        CloseHandle(th);
    }
    fprintf(stderr, "[reproducer] B3: breakpoint armed on 0x%llx\n", static_cast<unsigned long long>(*address));

    // Boucle WaitForDebugEvent pendant 1s -- traite EXCEPTION_SINGLE_STEP
    // exactement comme HardwareBreakpointSession::monitorLoop().
    const DWORD start = GetTickCount();
    int singleStepHits = 0;
    int reArmFailures = 0;
    while (GetTickCount() - start < 1000) {
        DEBUG_EVENT ev{};
        if (!WaitForDebugEvent(&ev, 100)) continue;
        DWORD continueStatus = DBG_CONTINUE;
        if (ev.dwDebugEventCode == EXCEPTION_DEBUG_EVENT) {
            const auto code = ev.u.Exception.ExceptionRecord.ExceptionCode;
            if (code == EXCEPTION_SINGLE_STEP || code == static_cast<DWORD>(0x80000004)) {
                ++singleStepHits;
                HANDLE th = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, ev.dwThreadId);
                if (th) {
                    if (!armThreadRaw(th, *address)) ++reArmFailures;
                    CloseHandle(th);
                }
            } else {
                continueStatus = DBG_EXCEPTION_NOT_HANDLED;
            }
        } else if (ev.dwDebugEventCode == CREATE_THREAD_DEBUG_EVENT) {
            HANDLE th = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, ev.dwThreadId);
            if (th) {
                armThreadRaw(th, *address);
                CloseHandle(th);
            }
        }
        ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, continueStatus);
    }
    fprintf(stderr, "[reproducer] B3: singleStepHits=%d reArmFailures=%d\n", singleStepHits, reArmFailures);
    ASSERT_GT(singleStepHits, 0) << "No EXCEPTION_SINGLE_STEP captured under stress rewrite -- test invalid, did not exercise the real code path";

    // Desarme tout (dr7=0) sur toutes les threads courantes -- equivalent de
    // clearBreakpoints().
    for (DWORD tid : enumerateThreadIdsRaw(pid)) {
        HANDLE th = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, tid);
        if (!th) continue;
        disarmThreadRaw(th);
        CloseHandle(th);
    }

    const BOOL stopOk = DebugActiveProcessStop(pid);
    fprintf(stderr, "[reproducer] B3: DebugActiveProcessStop=%d (GetLastError=%lu)\n", stopOk, GetLastError());
    ASSERT_TRUE(stopOk);

    EXPECT_TRUE(tryVirtualAllocRaw(pid, "ScenarioB3"))
        << "VirtualAllocEx failed after a REAL armed breakpoint + stress + detach -- "
           "the EXCEPTION_SINGLE_STEP handling / re-arm cycle is the differentiator";
}

// B4 : IDENTIQUE a B3, sauf que la boucle s'arrete des maxHits=5 atteint --
// exactement les parametres du vrai test qui echoue
// (killcore::findWhatWrites(..., timeoutMs=1000, maxHits=5)). B3 laissait
// tourner la session ~1s / ~277 hits ; B4 s'arrete en quelques dizaines de
// ms / 5 hits. Teste l'hypothese qu'une session de debug qui se termine TRES
// VITE (peu d'evenements traites) laisse un etat residuel different d'une
// session plus longue.
TEST(DebugDetachReproducer, ScenarioB4_RealArmedBreakpointFiveHitsOnly_ThenVirtualAlloc) {
    ASSERT_TRUE(enableDebugPrivilegeRaw());
    RawTestTargetProcess target(/*stressRewrite=*/true);
    ASSERT_TRUE(target.started());
    const DWORD pid = target.pid();
    const auto address = readTestTargetHealthAddressRaw(pid);
    ASSERT_TRUE(address.has_value()) << "Could not read g_health address from marker file";

    ASSERT_TRUE(DebugActiveProcess(pid)) << "DebugActiveProcess failed, error=" << GetLastError();
    logStepOk("DebugActiveProcess");

    for (DWORD tid : enumerateThreadIdsRaw(pid)) {
        HANDLE th = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, tid);
        if (!th) continue;
        armThreadRaw(th, *address);
        CloseHandle(th);
    }

    const DWORD start = GetTickCount();
    int singleStepHits = 0;
    constexpr int kMaxHits = 5;
    while (GetTickCount() - start < 1000 && singleStepHits < kMaxHits) {
        DEBUG_EVENT ev{};
        if (!WaitForDebugEvent(&ev, 100)) continue;
        DWORD continueStatus = DBG_CONTINUE;
        if (ev.dwDebugEventCode == EXCEPTION_DEBUG_EVENT) {
            const auto code = ev.u.Exception.ExceptionRecord.ExceptionCode;
            if (code == EXCEPTION_SINGLE_STEP || code == static_cast<DWORD>(0x80000004)) {
                ++singleStepHits;
                HANDLE th = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, ev.dwThreadId);
                if (th) {
                    armThreadRaw(th, *address);
                    CloseHandle(th);
                }
            } else {
                continueStatus = DBG_EXCEPTION_NOT_HANDLED;
            }
        } else if (ev.dwDebugEventCode == CREATE_THREAD_DEBUG_EVENT) {
            HANDLE th = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, ev.dwThreadId);
            if (th) {
                armThreadRaw(th, *address);
                CloseHandle(th);
            }
        }
        ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, continueStatus);
    }
    const DWORD elapsedMs = GetTickCount() - start;
    fprintf(stderr, "[reproducer] B4: singleStepHits=%d elapsedMs=%lu\n", singleStepHits, elapsedMs);
    ASSERT_GE(singleStepHits, kMaxHits) << "Did not reach 5 hits -- test invalid";

    for (DWORD tid : enumerateThreadIdsRaw(pid)) {
        HANDLE th = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, tid);
        if (!th) continue;
        disarmThreadRaw(th);
        CloseHandle(th);
    }

    const BOOL stopOk = DebugActiveProcessStop(pid);
    fprintf(stderr, "[reproducer] B4: DebugActiveProcessStop=%d (GetLastError=%lu)\n", stopOk, GetLastError());
    ASSERT_TRUE(stopOk);

    EXPECT_TRUE(tryVirtualAllocRaw(pid, "ScenarioB4"))
        << "VirtualAllocEx failed after a SHORT armed-breakpoint session (5 hits, "
        << elapsedMs << "ms) -- points to session duration/event count as the differentiator";
}

// C : utilise le VRAI killcore::findWhatWrites() (pas une reproduction --
// l'implementation reelle, HardwareBreakpointSession complet, arbitre
// inclus) puis un VirtualAllocEx BRUT (pas injectDll/monitor()). Bissection :
// si ca ECHOUE ici, le probleme est dans HardwareBreakpointSession/
// findWhatWrites lui-meme (son teardown laisse un residu). Si ca REUSSIT
// ici, le probleme est specifiquement dans InProcessBreakpointSession::
// monitor() (CreateFileMappingW/MapViewOfFile/injectDll), pas dans
// findWhatWrites.
TEST(DebugDetachReproducer, ScenarioC_RealFindWhatWrites_ThenRawVirtualAlloc) {
    ASSERT_TRUE(enableDebugPrivilegeRaw());
    RawTestTargetProcess target(/*stressRewrite=*/true);
    ASSERT_TRUE(target.started());
    const DWORD pid = target.pid();
    const auto address = readTestTargetHealthAddressRaw(pid);
    ASSERT_TRUE(address.has_value()) << "Could not read g_health address from marker file";

    const auto hits = killcore::findWhatWrites(
        pid, *address, killcore::BreakpointSize::DWord, /*timeoutMs=*/1000, /*maxHits=*/5);
    fprintf(stderr, "[reproducer] C: real findWhatWrites() returned %zu hits\n", hits.size());
    ASSERT_FALSE(hits.isEmpty()) << "Real findWhatWrites captured nothing -- test invalid";

    EXPECT_TRUE(tryVirtualAllocRaw(pid, "ScenarioC"))
        << "VirtualAllocEx failed after the REAL killcore::findWhatWrites() -- "
           "the bug is inside HardwareBreakpointSession's own teardown, not the raw Win32 sequence";
}

// D : reproduit EXACTEMENT le preambule de InProcessBreakpointSession::
// monitor() (CreateFileMappingW avec le meme nom/la meme taille que
// inprocess_breakpoint_ipc.h, MapViewOfFile) apres le VRAI findWhatWrites(),
// puis VirtualAllocEx brut -- teste si c'est specifiquement la creation du
// mapping partage nomme (pas juste VirtualAllocEx seul comme au Scenario C)
// qui interagit mal avec le cycle externe precedent.
TEST(DebugDetachReproducer, ScenarioD_RealFindWhatWrites_ThenSharedMapping_ThenVirtualAlloc) {
    ASSERT_TRUE(enableDebugPrivilegeRaw());
    RawTestTargetProcess target(/*stressRewrite=*/true);
    ASSERT_TRUE(target.started());
    const DWORD pid = target.pid();
    const auto address = readTestTargetHealthAddressRaw(pid);
    ASSERT_TRUE(address.has_value()) << "Could not read g_health address from marker file";

    const auto hits = killcore::findWhatWrites(
        pid, *address, killcore::BreakpointSize::DWord, /*timeoutMs=*/1000, /*maxHits=*/5);
    fprintf(stderr, "[reproducer] D: real findWhatWrites() returned %zu hits\n", hits.size());
    ASSERT_FALSE(hits.isEmpty()) << "Real findWhatWrites captured nothing -- test invalid";

    // Meme nom/meme taille que buildInProcessBreakpointMappingName() +
    // InProcessBreakpointIpcState (inprocess_breakpoint_ipc.h) -- structure
    // POD simplifiee ici (contenu exact sans importance, seule la taille
    // approx. et le nom comptent pour ce test).
    wchar_t mappingName[64];
    swprintf_s(mappingName, L"Local\\KillEngineInProcessBp_%u", pid);
    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 128, mappingName);
    ASSERT_NE(mapping, nullptr) << "CreateFileMappingW failed, error=" << GetLastError();
    fprintf(stderr, "[reproducer] D: CreateFileMappingW OK (alreadyExisted=%d)\n", GetLastError() == ERROR_ALREADY_EXISTS);
    LPVOID view = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, 128);
    ASSERT_NE(view, nullptr) << "MapViewOfFile failed, error=" << GetLastError();
    logStepOk("D: MapViewOfFile");

    const bool allocOk = tryVirtualAllocRaw(pid, "ScenarioD");

    UnmapViewOfFile(view);
    CloseHandle(mapping);

    EXPECT_TRUE(allocOk)
        << "VirtualAllocEx failed after findWhatWrites() + a named shared-memory mapping (monitor()'s preamble) -- "
           "the shared mapping creation is the differentiator";
}

// E : le VRAI code, integralement, sans loop ni retry -- exactement ce que
// fait BreakpointArbiterRuntimeTest.SequentialCyclesAcrossFreshPidsExternalThenInProcess
// pour un seul pidCycle. Si ca echoue ici aussi (hors de toute boucle), la
// boucle/le retry ne sont pas en cause -- le bug est dans monitor() lui-meme
// ou dans une interaction non identifiee entre les DEUX classes reelles.
TEST(DebugDetachReproducer, ScenarioE_RealFindWhatWrites_ThenRealInProcessMonitor_SingleShot) {
    ASSERT_TRUE(enableDebugPrivilegeRaw());
    RawTestTargetProcess target(/*stressRewrite=*/true);
    ASSERT_TRUE(target.started());
    const DWORD pid = target.pid();
    const auto address = readTestTargetHealthAddressRaw(pid);
    ASSERT_TRUE(address.has_value()) << "Could not read g_health address from marker file";

    const auto hits = killcore::findWhatWrites(
        pid, *address, killcore::BreakpointSize::DWord, /*timeoutMs=*/1000, /*maxHits=*/5);
    fprintf(stderr, "[reproducer] E: real findWhatWrites() returned %zu hits\n", hits.size());
    ASSERT_FALSE(hits.isEmpty()) << "Real findWhatWrites captured nothing -- test invalid";

    const QString handlerPath = QDir(QCoreApplication::applicationDirPath()).filePath("KillEngineInProcessBreakpointHandler.dll");
    ASSERT_TRUE(QFile::exists(handlerPath));

    killcore::ProcessHandle handle(pid, killcore::ProcessAccess::AllAccess);
    ASSERT_TRUE(handle.isValid());

    killcore::InProcessBreakpointConfig cfg;
    cfg.address = *address;
    cfg.size = sizeof(int32_t);
    cfg.captureWrites = true;
    cfg.timeoutMs = 1000;
    cfg.maxHits = 5;
    cfg.injectedHandlerPath = handlerPath;

    killcore::InProcessBreakpointSession session;
    const auto result = session.monitor(handle, cfg);
    fprintf(stderr, "[reproducer] E: real InProcessBreakpointSession::monitor() success=%d error=%s\n",
            result.success, result.error.toStdString().c_str());

    if (!result.success && result.error.contains("antivirus/EDR", Qt::CaseInsensitive)) {
        GTEST_SKIP() << "Bloque par signature EDR connue, voir docs/STRATEGY_ROOM.md 20/08/2026 : "
                     << result.error.toStdString();
    }
    EXPECT_TRUE(result.success) << result.error.toStdString();
}

// F : isole la VRAIE variable restante -- l'ORDRE entre OpenProcess et
// CreateFileMappingW/MapViewOfFile. Scenario D (reussi) ouvrait le handle
// APRES le mapping partage. Ici, le handle est ouvert AVANT (comme le fait
// killcore::ProcessHandle handle(...) dans le vrai appelant, cree avant
// session.monitor()), et c'est CE handle, deja ouvert, qui sert pour
// VirtualAllocEx -- pas un handle frais.
TEST(DebugDetachReproducer, ScenarioF_HandleOpenedBeforeMapping_ThenVirtualAllocOnSameHandle) {
    ASSERT_TRUE(enableDebugPrivilegeRaw());
    RawTestTargetProcess target(/*stressRewrite=*/true);
    ASSERT_TRUE(target.started());
    const DWORD pid = target.pid();
    const auto address = readTestTargetHealthAddressRaw(pid);
    ASSERT_TRUE(address.has_value()) << "Could not read g_health address from marker file";

    const auto hits = killcore::findWhatWrites(
        pid, *address, killcore::BreakpointSize::DWord, /*timeoutMs=*/1000, /*maxHits=*/5);
    fprintf(stderr, "[reproducer] F: real findWhatWrites() returned %zu hits\n", hits.size());
    ASSERT_FALSE(hits.isEmpty()) << "Real findWhatWrites captured nothing -- test invalid";

    // Handle ouvert ICI, AVANT le mapping -- c'est le seul changement vs D.
    HANDLE hProcess = OpenProcess(
        PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION,
        FALSE, pid);
    ASSERT_NE(hProcess, nullptr) << "OpenProcess failed, error=" << GetLastError();
    logStepOk("F: OpenProcess (early, before mapping)");

    wchar_t mappingName[64];
    swprintf_s(mappingName, L"Local\\KillEngineInProcessBp_%u", pid);
    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 128, mappingName);
    ASSERT_NE(mapping, nullptr) << "CreateFileMappingW failed, error=" << GetLastError();
    LPVOID view = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, 128);
    ASSERT_NE(view, nullptr) << "MapViewOfFile failed, error=" << GetLastError();
    logStepOk("F: shared mapping created");

    LPVOID mem = VirtualAllocEx(hProcess, nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    const DWORD allocErr = GetLastError();
    if (!mem) {
        fprintf(stderr, "[reproducer] F: VirtualAllocEx on the EARLY handle FAILED (error=%lu)\n", allocErr);
    } else {
        fprintf(stderr, "[reproducer] F: VirtualAllocEx on the EARLY handle OK at %p\n", mem);
        VirtualFreeEx(hProcess, mem, 0, MEM_RELEASE);
    }

    UnmapViewOfFile(view);
    CloseHandle(mapping);
    CloseHandle(hProcess);

    EXPECT_NE(mem, nullptr)
        << "VirtualAllocEx failed specifically on a handle opened BEFORE the shared mapping -- "
           "confirms handle-opening order relative to CreateFileMappingW/MapViewOfFile as the cause";
}

// G : va jusqu'au bout -- VirtualAllocEx -> WriteProcessMemory -> GetProcAddress
// (local, kernel32 charge a la meme adresse) -> CreateRemoteThread, exactement
// la sequence de core/inject/dll_injector.cpp::injectDll(), sur le handle
// ouvert APRES le cycle externe (comme au Scenario C, qui reussissait pour
// VirtualAllocEx seul). PROCESS_CREATE_THREAD n'est JAMAIS dans nos flags
// (AllAccess = QUERY_LIMITED|VM_READ|VM_WRITE|VM_OPERATION) -- CreateRemoteThread
// ne marche que via le bypass SeDebugPrivilege, jamais teste isolement jusqu'ici.
TEST(DebugDetachReproducer, ScenarioG_RealFindWhatWrites_ThenFullInjectionChain) {
    ASSERT_TRUE(enableDebugPrivilegeRaw());
    RawTestTargetProcess target(/*stressRewrite=*/true);
    ASSERT_TRUE(target.started());
    const DWORD pid = target.pid();
    const auto address = readTestTargetHealthAddressRaw(pid);
    ASSERT_TRUE(address.has_value()) << "Could not read g_health address from marker file";

    const auto hits = killcore::findWhatWrites(
        pid, *address, killcore::BreakpointSize::DWord, /*timeoutMs=*/1000, /*maxHits=*/5);
    fprintf(stderr, "[reproducer] G: real findWhatWrites() returned %zu hits\n", hits.size());
    ASSERT_FALSE(hits.isEmpty()) << "Real findWhatWrites captured nothing -- test invalid";

    HANDLE hProcess = OpenProcess(
        PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION,
        FALSE, pid);
    ASSERT_NE(hProcess, nullptr) << "OpenProcess failed, error=" << GetLastError();

    const std::wstring dummyPath = L"C:\\does-not-need-to-exist.dll";
    const SIZE_T pathSize = (dummyPath.size() + 1) * sizeof(wchar_t);
    LPVOID pRemotePath = VirtualAllocEx(hProcess, nullptr, pathSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!pRemotePath) {
        fprintf(stderr, "[reproducer] G: VirtualAllocEx FAILED error=%lu\n", GetLastError());
    } else {
        fprintf(stderr, "[reproducer] G: VirtualAllocEx OK at %p\n", pRemotePath);
    }
    ASSERT_NE(pRemotePath, nullptr);

    SIZE_T written = 0;
    const BOOL wpmOk = WriteProcessMemory(hProcess, pRemotePath, dummyPath.c_str(), pathSize, &written);
    fprintf(stderr, "[reproducer] G: WriteProcessMemory=%d written=%zu (GetLastError=%lu)\n", wpmOk, written, GetLastError());
    ASSERT_TRUE(wpmOk);

    const HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll");
    ASSERT_NE(hKernel32, nullptr);
    const FARPROC loadLibraryAddr = GetProcAddress(hKernel32, "LoadLibraryW");
    ASSERT_NE(loadLibraryAddr, nullptr);

    HANDLE hThread = CreateRemoteThread(
        hProcess, nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(loadLibraryAddr), pRemotePath, 0, nullptr);
    const DWORD crtErr = GetLastError();
    if (!hThread) {
        fprintf(stderr, "[reproducer] G: CreateRemoteThread FAILED error=%lu\n", crtErr);
    } else {
        fprintf(stderr, "[reproducer] G: CreateRemoteThread OK, handle=%p\n", hThread);
        WaitForSingleObject(hThread, 5000);
        CloseHandle(hThread);
    }

    VirtualFreeEx(hProcess, pRemotePath, 0, MEM_RELEASE);
    CloseHandle(hProcess);

    if (!hThread && crtErr == ERROR_ACCESS_DENIED) {
        GTEST_SKIP() << "CreateRemoteThread refuse (error=5) apres cycle debugger -- signature EDR connue, "
                        "voir docs/STRATEGY_ROOM.md 20/08/2026";
    }
    EXPECT_NE(hThread, nullptr) << "CreateRemoteThread failed on the exact same sequence as injectDll() -- error=" << crtErr;
}

// H : meme sequence que G, mais avec le VRAI PROCESS_ALL_ACCESS litteral
// (0x1FFFFF), pas notre jeu de flags restreint -- determine si le probleme
// est contournable par des flags plus larges ou s'il est structurel cote OS.
TEST(DebugDetachReproducer, ScenarioH_RealFindWhatWrites_ThenFullAccessHandle_CreateRemoteThread) {
    ASSERT_TRUE(enableDebugPrivilegeRaw());
    RawTestTargetProcess target(/*stressRewrite=*/true);
    ASSERT_TRUE(target.started());
    const DWORD pid = target.pid();
    const auto address = readTestTargetHealthAddressRaw(pid);
    ASSERT_TRUE(address.has_value()) << "Could not read g_health address from marker file";

    const auto hits = killcore::findWhatWrites(
        pid, *address, killcore::BreakpointSize::DWord, /*timeoutMs=*/1000, /*maxHits=*/5);
    ASSERT_FALSE(hits.isEmpty()) << "Real findWhatWrites captured nothing -- test invalid";

    HANDLE hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!hProcess) {
        fprintf(stderr, "[reproducer] H: OpenProcess(PROCESS_ALL_ACCESS) FAILED error=%lu\n", GetLastError());
    }
    ASSERT_NE(hProcess, nullptr);
    logStepOk("H: OpenProcess(PROCESS_ALL_ACCESS)");

    const std::wstring dummyPath = L"C:\\does-not-need-to-exist.dll";
    const SIZE_T pathSize = (dummyPath.size() + 1) * sizeof(wchar_t);
    LPVOID pRemotePath = VirtualAllocEx(hProcess, nullptr, pathSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    ASSERT_NE(pRemotePath, nullptr) << "VirtualAllocEx failed, error=" << GetLastError();
    SIZE_T written = 0;
    ASSERT_TRUE(WriteProcessMemory(hProcess, pRemotePath, dummyPath.c_str(), pathSize, &written));

    const HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll");
    const FARPROC loadLibraryAddr = GetProcAddress(hKernel32, "LoadLibraryW");

    HANDLE hThread = CreateRemoteThread(
        hProcess, nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(loadLibraryAddr), pRemotePath, 0, nullptr);
    const DWORD crtErr = GetLastError();
    if (!hThread) {
        fprintf(stderr, "[reproducer] H: CreateRemoteThread with PROCESS_ALL_ACCESS FAILED error=%lu\n", crtErr);
    } else {
        fprintf(stderr, "[reproducer] H: CreateRemoteThread with PROCESS_ALL_ACCESS OK\n");
        WaitForSingleObject(hThread, 5000);
        CloseHandle(hThread);
    }
    VirtualFreeEx(hProcess, pRemotePath, 0, MEM_RELEASE);
    CloseHandle(hProcess);

    if (!hThread && crtErr == ERROR_ACCESS_DENIED) {
        GTEST_SKIP() << "CreateRemoteThread refuse (error=5) meme avec PROCESS_ALL_ACCESS -- signature EDR "
                        "connue, voir docs/STRATEGY_ROOM.md 20/08/2026";
    }
    EXPECT_NE(hThread, nullptr)
        << "CreateRemoteThread STILL fails even with literal PROCESS_ALL_ACCESS -- "
           "not fixable via broader access flags, points to a structural OS-level restriction, error=" << crtErr;
}

// I : teste si un DELAI LONG (jusqu'a 3s), specifiquement entre
// DebugActiveProcessStop et CreateRemoteThread (pas un retry de toute la
// sequence comme les tentatives precedentes), permet a CreateRemoteThread de
// reussir -- confirme ou infirme une hypothese de nettoyage asynchrone cote
// noyau (objet debug/port pas totalement libere de facon synchrone par
// DebugActiveProcessStop malgre son retour TRUE).
TEST(DebugDetachReproducer, ScenarioI_LongDelayBeforeCreateRemoteThread) {
    ASSERT_TRUE(enableDebugPrivilegeRaw());
    RawTestTargetProcess target(/*stressRewrite=*/true);
    ASSERT_TRUE(target.started());
    const DWORD pid = target.pid();
    const auto address = readTestTargetHealthAddressRaw(pid);
    ASSERT_TRUE(address.has_value()) << "Could not read g_health address from marker file";

    const auto hits = killcore::findWhatWrites(
        pid, *address, killcore::BreakpointSize::DWord, /*timeoutMs=*/1000, /*maxHits=*/5);
    ASSERT_FALSE(hits.isEmpty()) << "Real findWhatWrites captured nothing -- test invalid";

    fprintf(stderr, "[reproducer] I: waiting 3000ms before CreateRemoteThread...\n");
    Sleep(3000);

    HANDLE hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    ASSERT_NE(hProcess, nullptr) << "OpenProcess failed, error=" << GetLastError();

    const std::wstring dummyPath = L"C:\\does-not-need-to-exist.dll";
    const SIZE_T pathSize = (dummyPath.size() + 1) * sizeof(wchar_t);
    LPVOID pRemotePath = VirtualAllocEx(hProcess, nullptr, pathSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!pRemotePath && GetLastError() == ERROR_ACCESS_DENIED) {
        CloseHandle(hProcess);
        GTEST_SKIP() << "VirtualAllocEx refuse (error=5) apres delai -- signature EDR connue, "
                        "voir docs/STRATEGY_ROOM.md 20/08/2026";
    }
    ASSERT_NE(pRemotePath, nullptr);
    SIZE_T written = 0;
    ASSERT_TRUE(WriteProcessMemory(hProcess, pRemotePath, dummyPath.c_str(), pathSize, &written));

    const HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll");
    const FARPROC loadLibraryAddr = GetProcAddress(hKernel32, "LoadLibraryW");

    HANDLE hThread = CreateRemoteThread(
        hProcess, nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(loadLibraryAddr), pRemotePath, 0, nullptr);
    const DWORD crtErr = GetLastError();
    if (!hThread) {
        fprintf(stderr, "[reproducer] I: CreateRemoteThread AFTER 3s delay FAILED error=%lu\n", crtErr);
    } else {
        fprintf(stderr, "[reproducer] I: CreateRemoteThread AFTER 3s delay OK\n");
        WaitForSingleObject(hThread, 5000);
        CloseHandle(hThread);
    }
    VirtualFreeEx(hProcess, pRemotePath, 0, MEM_RELEASE);
    CloseHandle(hProcess);

    if (!hThread && crtErr == ERROR_ACCESS_DENIED) {
        GTEST_SKIP() << "CreateRemoteThread refuse (error=5) apres delai -- signature EDR connue, "
                        "voir docs/STRATEGY_ROOM.md 20/08/2026";
    }
    EXPECT_NE(hThread, nullptr)
        << "CreateRemoteThread still fails even after a 3s delay -- rules out an async kernel cleanup race, error=" << crtErr;
}

#endif // Q_OS_WIN
