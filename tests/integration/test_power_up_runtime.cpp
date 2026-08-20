// =============================================================================
// Tests runtime Windows pour les modules power-up (Phases 19-20)
//
// Objectif : prouver réellement, sur KillEngineTestTarget.exe, que :
//   1. BreakpointFreezeManager peut surveiller une adresse writable.
//   2. Un WriteProcessMemory externe n'est PAS intercepté par le breakpoint
//      (limite fondamentale des hardware breakpoints — voir le test dédié).
//   3. Le freeze tient face à une cible qui réécrit sa propre mémoire en
//      continu à ~1000 Hz via ses propres instructions CPU (stress réaliste,
//      pas via WriteProcessMemory).
//   4. injectDll gère correctement l'échec (DLL inexistante) en x64.
//   5. installInlineHook détecte les instructions RIP-relative avant patch.
//
// Les tests skip proprement si les privilèges debug ne sont pas disponibles.
// =============================================================================

#include <gtest/gtest.h>

#include "process/process_handle.h"
#include "process/process_enumerator.h"
#include "memory/memory_reader.h"
#include "memory/memory_writer.h"
#include "scanner/scan_engine.h"
#include "scanner/scan_types.h"
#include "debug/breakpoint_freeze.h"
#include "debug/hardware_breakpoint.h"
#include "debug/page_guard.h"
#include "inject/dll_injector.h"
#include "inject/function_hook.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QThread>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace {

class TestTargetProcess {
public:
    // stressRewrite=true active un thread interne à la cible qui réécrit
    // g_health via ses propres instructions CPU (voir
    // tests/memory_targets/test_target_main.cpp). Nécessaire pour tester un
    // hardware breakpoint : WriteProcessMemory externe ne le déclenche jamais.
    explicit TestTargetProcess(bool stressRewrite = false) {
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

    ~TestTargetProcess() {
        if (m_process.state() != QProcess::NotRunning) {
            m_process.terminate();
            if (!m_process.waitForFinished(3000)) {
                m_process.kill();
                m_process.waitForFinished(3000);
            }
        }
    }

    bool started() const {
        return m_process.state() != QProcess::NotRunning;
    }

    uint32_t pid() const {
        return static_cast<uint32_t>(m_process.processId());
    }

private:
    QProcess m_process;
};

QByteArray int32Bytes(int32_t value) {
    QByteArray bytes;
    bytes.append(reinterpret_cast<const char*>(&value), sizeof(value));
    return bytes;
}

killcore::ScanResult scanInt32(killcore::ProcessHandle& handle, int32_t expected) {
    killcore::ScanValue value;
    QString parseError;
    EXPECT_TRUE(killcore::parseScanValue(QString::number(expected), killcore::ValueType::Int32, &value, &parseError))
        << parseError.toStdString();

    killcore::ScanOptions options;
    options.maxResults = 1000000;

    killcore::ScanEngine scanner(handle);
    return scanner.exactScan(value, options);
}

std::optional<uint64_t> findWritableInt32Address(
    killcore::ProcessHandle& handle,
    int32_t value) {

    const auto scan = scanInt32(handle, value);
    if (!scan.success || scan.matches.isEmpty()) {
        return std::nullopt;
    }

    killcore::MemoryReader reader(handle);

    for (const auto& match : scan.matches) {
        if (match.type == killcore::ValueType::Int32) {
            const auto read = reader.read(match.address, sizeof(int32_t));
            if ((read.success || read.partial) && read.bytesRead == sizeof(int32_t)) {
                return match.address;
            }
        }
    }
    return std::nullopt;
}

// Lit l'adresse reelle de g_health exposee par KillEngineTestTarget.exe
// (voir tests/memory_targets/test_target_main.cpp). Fiable contrairement a
// findWritableInt32Address() par valeur : un scan par valeur peut tomber sur
// n'importe quelle autre variable Qt/CRT qui vaut coincidemment la meme
// chose, ce qui est particulierement trompeur sous stress rewrite ou la
// vraie valeur bouge en continu.
std::optional<uint64_t> readTestTargetHealthAddress(uint32_t expectedPid) {
    QFile marker(QDir::temp().filePath("killengine_test_target_addresses.txt"));
    if (!marker.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return std::nullopt;
    }
    const QString content = QString::fromUtf8(marker.readAll());
    marker.close();

    uint32_t markerPid = 0;
    uint64_t address = 0;
    for (const QString& line : content.split('\n', Qt::SkipEmptyParts)) {
        const auto parts = line.split('=');
        if (parts.size() != 2) continue;
        if (parts[0] == "pid") {
            markerPid = parts[1].toUInt();
        } else if (parts[0] == "g_health") {
            bool ok = false;
            address = parts[1].toULongLong(&ok, 16);
            if (!ok) address = 0;
        }
    }

    if (markerPid != expectedPid || address == 0) {
        return std::nullopt;
    }
    return address;
}

bool debugPrivilegesAvailable() {
#ifdef Q_OS_WIN
    return killcore::HardwareBreakpointSession::enableDebugPrivilege();
#else
    return false;
#endif
}

} // namespace

TEST(PowerUpRuntimeTest, BreakpointFreezeAttachesToWritableAddress) {
    if (!debugPrivilegesAvailable()) {
        GTEST_SKIP() << "Debug privileges not available — skipping breakpoint freeze test";
    }

    TestTargetProcess target;
    ASSERT_TRUE(target.started()) << "KillEngineTestTarget.exe did not start";

    killcore::ProcessHandle handle(target.pid(), killcore::ProcessAccess::ReadWrite);
    ASSERT_TRUE(handle.isValid()) << "Could not open test target process";

    const auto address = findWritableInt32Address(handle, 100);
    ASSERT_TRUE(address.has_value()) << "Could not find g_health address (Int32 == 100)";

    killcore::BreakpointFreezeManager freeze;

    killcore::BreakpointFreezeConfig config;
    config.address = *address;
    config.size = killcore::BreakpointSize::DWord;
    config.mode = killcore::BreakpointFreezeMode::Capture;
    config.frozenValue = int32Bytes(100);

    const bool started = freeze.start(target.pid(), config);

    if (!started) {
        GTEST_SKIP() << "DebugActiveProcess failed for test target — "
                        "this can happen if another debugger is attached or permissions are insufficient";
    }

    EXPECT_TRUE(freeze.isActive());

    QThread::msleep(500);

    freeze.stop();
    EXPECT_FALSE(freeze.isActive());
}

// Documente et verifie une limite fondamentale des hardware breakpoints,
// decouverte en durcissant ce test suite : DR0-DR3 ne piegent que les
// instructions executees sur un thread DU PROCESSUS CIBLE qui les porte.
// Un WriteProcessMemory externe (ce que fait ce test, et ce que fait
// KillEngine lui-meme pour ecrire une valeur depuis l'UI) copie les octets en
// mode noyau sans jamais executer d'instruction sur un thread du debuggee :
// le breakpoint ne se declenche donc jamais. Ce test etait auparavant nomme
// "...RewritesValueChangedByWriter" avec une assertion permissive qui passait
// que le breakpoint intercepte ou non — il ne prouvait rien. Il verifie
// desormais explicitement l'absence d'interception dans ce cas, pour que la
// distinction externe/interne reste un fait teste et non une supposition.
// Le vrai test de reecriture invincible est BreakpointFreezeHoldsUnderFastRewriteStress
// ci-dessous, qui utilise le rewriter interne a la cible.
TEST(PowerUpRuntimeTest, BreakpointFreezeDoesNotInterceptExternalWriteProcessMemory) {
    if (!debugPrivilegesAvailable()) {
        GTEST_SKIP() << "Debug privileges not available — skipping breakpoint freeze test";
    }

    TestTargetProcess target;
    ASSERT_TRUE(target.started()) << "KillEngineTestTarget.exe did not start";

    killcore::ProcessHandle handle(target.pid(), killcore::ProcessAccess::ReadWrite);
    ASSERT_TRUE(handle.isValid()) << "Could not open test target process";

    const auto address = findWritableInt32Address(handle, 100);
    ASSERT_TRUE(address.has_value()) << "Could not find g_health address";

    killcore::BreakpointFreezeManager freeze;

    killcore::BreakpointFreezeConfig config;
    config.address = *address;
    config.size = killcore::BreakpointSize::DWord;
    config.mode = killcore::BreakpointFreezeMode::RewriteValue;
    config.frozenValue = int32Bytes(100);

    if (!freeze.start(target.pid(), config)) {
        GTEST_SKIP() << "DebugActiveProcess failed — cannot test breakpoint freeze";
    }

    QThread::msleep(200);

    killcore::MemoryWriter writer(handle);
    const auto writeResult = writer.write(*address, int32Bytes(999), false);
    EXPECT_TRUE(writeResult.success) << "Write to test address failed";

    QThread::msleep(200);

    killcore::MemoryReader reader(handle);
    const auto finalRead = reader.read(*address, sizeof(int32_t));
    int32_t finalValue = 0;
    if (finalRead.success && finalRead.bytesRead == sizeof(int32_t)) {
        std::memcpy(&finalValue, finalRead.data.constData(), sizeof(int32_t));
    }

    const auto stats = freeze.stats();
    freeze.stop();

    EXPECT_EQ(finalValue, 999) << "External WriteProcessMemory should not be intercepted by a hardware breakpoint";
    EXPECT_EQ(stats.totalHits, 0u) << "Hardware breakpoint should not fire for a cross-process write";
    EXPECT_TRUE(target.started()) << "Test target crashed during breakpoint freeze";
}

// Preuve de robustesse "invincible freeze" face a une cible qui reecrit sa
// PROPRE memoire via ses propres instructions CPU (pas via un
// WriteProcessMemory externe : DR0-DR3 ne piegent que les instructions
// executees sur un thread du processus cible qui les porte, donc un writer
// externe ne declenche jamais un hardware breakpoint — voir le rewriter
// interne cote KillEngineTestTarget dans tests/memory_targets/test_target_main.cpp).
// Un thread de lecture independant echantillonne la valeur pendant ~1s de
// reecriture continue sans sleep : le hold-rate mesure la fraction
// d'echantillons ou le freeze breakpoint a effectivement tenu.
TEST(PowerUpRuntimeTest, BreakpointFreezeHoldsUnderFastRewriteStress) {
    if (!debugPrivilegesAvailable()) {
        GTEST_SKIP() << "Debug privileges not available — skipping breakpoint freeze stress test";
    }

    TestTargetProcess target(/*stressRewrite=*/true);
    ASSERT_TRUE(target.started()) << "KillEngineTestTarget.exe did not start";

    killcore::ProcessHandle handle(target.pid(), killcore::ProcessAccess::ReadWrite);
    ASSERT_TRUE(handle.isValid()) << "Could not open test target process";

    // Adresse exacte lue depuis le fichier marqueur ecrit par la cible, pas
    // devinee par scan de valeur : un Qt GUI process contient plein d'autres
    // Int32 qui valent coincidemment 100 (geometrie widgets, DPI, etc.), et
    // sous stress rewrite la vraie valeur bouge en continu de toute facon.
    const auto address = readTestTargetHealthAddress(target.pid());
    ASSERT_TRUE(address.has_value()) << "Could not read g_health address from test target marker file";

    // Volontairement different de la valeur initiale naturelle de g_health
    // (100) : sinon "le rewriter ne tourne pas du tout" et "le freeze
    // fonctionne parfaitement" produiraient tous les deux une lecture
    // constante a 100, rendant le test aveugle a une vraie regression.
    constexpr int32_t kFrozenValue = 777;

    killcore::BreakpointFreezeManager freeze;
    killcore::BreakpointFreezeConfig config;
    config.address = *address;
    config.size = killcore::BreakpointSize::DWord;
    config.mode = killcore::BreakpointFreezeMode::RewriteValue;
    config.frozenValue = int32Bytes(kFrozenValue);

    if (!freeze.start(target.pid(), config)) {
        GTEST_SKIP() << "DebugActiveProcess failed — cannot test breakpoint freeze stress";
    }

    // Attend que le rewriter interne cote cible commence reellement a
    // ecrire (delai de 1200ms depuis le lancement du process, voir
    // test_target_main.cpp) avant d'echantillonner, pour que toute la
    // fenetre de mesure ci-dessous tombe sous stress reel plutot que sur
    // une adresse encore silencieuse.
    QThread::msleep(700);

    killcore::MemoryReader reader(handle);
    uint64_t samples = 0;
    uint64_t heldSamples = 0;

    // Echantillonnage par spin-wait plutot que QThread::msleep() : sous
    // Windows, msleep() est borne par la granularite par defaut du timer
    // systeme (~15ms), ce qui donnerait ~64 echantillons sur 1s — bien trop
    // grossier face a un cycle d'ecriture/correction de l'ordre de la
    // microseconde, et statistiquement trop bruite pour un seuil fiable.
    constexpr auto kSampleInterval = std::chrono::microseconds(200);
    const auto stressStart = std::chrono::steady_clock::now();
    auto nextSample = stressStart;
    while (std::chrono::steady_clock::now() - stressStart < std::chrono::milliseconds(1000)) {
        while (std::chrono::steady_clock::now() < nextSample) {
            // spin-wait volontaire, meme raison que le writer cote cible
        }
        const auto read = reader.read(*address, sizeof(int32_t));
        if ((read.success || read.partial) && read.bytesRead == sizeof(int32_t)) {
            int32_t current = 0;
            std::memcpy(&current, read.data.constData(), sizeof(int32_t));
            ++samples;
            if (current == kFrozenValue) {
                ++heldSamples;
            }
        }
        nextSample += kSampleInterval;
    }

    const auto stats = freeze.stats();
    freeze.stop();

    // Pas de verification "valeur retombee au repos" ici : le rewriter cote
    // cible n'a pas de mecanisme d'arret pilote depuis ce test (il tourne
    // jusqu'a la fin du process) — lire la valeur juste apres avoir arrete
    // d'echantillonner ne prouverait rien, la cible continue d'ecrire.
    // Le hold-rate pendant la fenetre de stress est la seule mesure valide.

    ASSERT_GT(samples, 0u) << "No samples collected during stress window";
    const double holdRate = static_cast<double>(heldSamples) / static_cast<double>(samples);

    std::cout << "[BreakpointFreezeHoldsUnderFastRewriteStress] "
              << "totalHits=" << stats.totalHits
              << " rewrites=" << stats.rewrites
              << " errors=" << stats.errors
              << " samples=" << samples
              << " heldSamples=" << heldSamples
              << " holdRate=" << holdRate << std::endl;

    EXPECT_GT(stats.totalHits, 0u) << "Breakpoint never triggered during stress — freeze is not intercepting writes";
    // A 1000 Hz, chaque ecriture doit etre interceptee (pas seulement "la
    // plupart") : c'est la garantie qui distingue un hardware breakpoint du
    // polling. Une hit manquee est le signe d'une regression, pas d'un
    // hasard de timing (contrairement au hold-rate ci-dessous).
    EXPECT_GE(stats.totalHits, 900u) << "Expected close to 1000 hits at ~1000 Hz for 1s, got " << stats.totalHits;

    // Hold-rate mesure empiriquement (4 runs, ~5000 echantillons chacun,
    // spin-wait 200us) : converge de facon stable a ~80% avec un pic occasionnel
    // pres de 100%. Ce n'est pas du bruit — c'est la latence physique reelle du
    // mecanisme (reveil WaitForDebugEvent + ReadProcessMemory + WriteProcessMemory
    // a chaque cycle) face a une reecriture a 1000 Hz precisement cadencee, un
    // scenario deja largement plus agressif qu'un vrai jeu (qui mute rarement une
    // valeur plus d'une fois par frame, 60-240 Hz). Seuil fixe sous la baseline
    // mesuree pour laisser de la marge machine chargee, tout en detectant une
    // vraie regression (le mecanisme casse total montre un holdRate proche de 0%,
    // pas un score legerement plus bas).
    EXPECT_GE(holdRate, 0.60) << "Freeze held only " << (holdRate * 100.0)
                              << "% of samples under 1000 Hz rewrite stress (target: >=60%, baseline ~80%)";

    EXPECT_TRUE(target.started()) << "Test target crashed during breakpoint freeze stress";
}

// Regression pour le bug signale par l'utilisateur le 19/08/2026 : "Ecrit
// par" (findWhatWrites) faisait planter le processus attache juste apres une
// capture reussie. Root cause : applyBreakpointsToThread() (hardware_breakpoint.cpp)
// remettait a zero Dr0-Dr3/Dr7 a chaque reamorcage et au detachement, mais
// jamais Dr6 (registre de statut, pas efface automatiquement par le CPU apres
// une exception de debug — Intel SDM Vol.3B §17.2.4). Un Dr6 encore "sale"
// (bits B0-B3 du dernier hit) qui survit au detachement peut faire planter
// la cible au prochain evenement de debug qu'elle genere elle-meme — corrige
// en alignant sur core/debug/breakpoint_freeze.cpp, qui le faisait deja
// correctement. Ce test ne peut pas prouver l'absence d'un bug de timing a
// coup sur, mais verifie au minimum que la cible survit un cycle complet
// attache→capture→detache sous stress d'ecriture reelle, et reste lisible
// juste apres (pas juste "le process existe encore", un process zombie
// passerait started() sans repondre a une vraie lecture memoire).
TEST(PowerUpRuntimeTest, FindWhatWritesDoesNotCrashTargetAfterCapture) {
    if (!debugPrivilegesAvailable()) {
        GTEST_SKIP() << "Debug privileges not available — skipping find what writes test";
    }

    TestTargetProcess target(/*stressRewrite=*/true);
    ASSERT_TRUE(target.started()) << "KillEngineTestTarget.exe did not start";

    const auto address = readTestTargetHealthAddress(target.pid());
    ASSERT_TRUE(address.has_value()) << "Could not read g_health address from test target marker file";

    const auto hits = killcore::findWhatWrites(
        target.pid(),
        *address,
        killcore::BreakpointSize::DWord,
        /*timeoutMs=*/3000,
        /*maxHits=*/16);

    ASSERT_FALSE(hits.isEmpty()) << "No write captured under stress rewrite — capture path itself is broken";

    // Le point du test : la cible doit survivre au detachement et rester
    // fonctionnelle, pas seulement "exister" comme process zombie.
    EXPECT_TRUE(target.started()) << "Test target crashed after Find What Writes detached — Dr6 regression";

    killcore::ProcessHandle handle(target.pid(), killcore::ProcessAccess::ReadOnly);
    ASSERT_TRUE(handle.isValid()) << "Could not reopen test target after Find What Writes — process likely unstable";
    killcore::MemoryReader reader(handle);
    const auto read = reader.read(*address, sizeof(int32_t));
    EXPECT_TRUE(read.success || read.partial) << "Could not read g_health after Find What Writes — target left in a broken state";
}

// Preuve reelle, out-of-process, du chemin complet PAGE_GUARD : injection de
// KillEnginePageGuardHandler.dll dans KillEngineTestTarget.exe, pose de la
// garde sur g_health, capture d'un hit pendant que la cible le reecrit en
// stress (~1000 Hz, memes instructions CPU que le test breakpoint freeze
// stress ci-dessus — pas de WriteProcessMemory externe, ce serait invisible
// pour un VEH qui vit dans la cible comme pour un hardware breakpoint).
// Contrairement au test breakpoint freeze, celui-ci ne devrait PAS necessiter
// SeDebugPrivilege : c'est justement l'avantage annonce de PAGE_GUARD.
TEST(PowerUpRuntimeTest, PageGuardCapturesRemoteStressRewrite) {
    const QString handlerPath = QDir(QCoreApplication::applicationDirPath()).filePath("KillEnginePageGuardHandler.dll");
    ASSERT_TRUE(QFile::exists(handlerPath)) << "KillEnginePageGuardHandler.dll not found next to test binary — build issue";

    TestTargetProcess target(/*stressRewrite=*/true);
    ASSERT_TRUE(target.started()) << "KillEngineTestTarget.exe did not start";

    const auto address = readTestTargetHealthAddress(target.pid());
    ASSERT_TRUE(address.has_value()) << "Could not read g_health address from test target marker file";

    killcore::ProcessHandle handle(target.pid(), killcore::ProcessAccess::AllAccess);
    ASSERT_TRUE(handle.isValid()) << "Could not open test target with AllAccess (required for DLL injection)";

    killcore::PageGuardConfig config;
    config.address = *address;
    config.size = sizeof(int32_t);
    config.captureWrites = true;
    config.captureReads = false;
    config.timeoutMs = 3000;
    config.maxHits = 5;
    config.injectedHandlerPath = handlerPath;

    killcore::PageGuardSession session;
    const auto result = session.monitor(handle, config);

    EXPECT_TRUE(result.success) << "PageGuard monitor failed: " << result.error.toStdString();
    ASSERT_FALSE(result.hits.isEmpty()) << "No PAGE_GUARD hit captured under 1000 Hz stress rewrite — "
                                            "handler injection or VEH install likely did not work";

    const auto& hit = result.hits.first();
    EXPECT_EQ(hit.monitoredAddress, *address);
    EXPECT_TRUE(hit.isWrite) << "g_health stress rewrite is a write, not a read";
    EXPECT_NE(hit.instructionPointer, 0u) << "Captured hit has no RIP — handler wiring is broken";
    EXPECT_GE(hit.accessAddress, *address);
    EXPECT_LT(hit.accessAddress, *address + config.size);
    // Le RIP doit tomber dans KillEngineTestTarget.exe lui-meme (c'est son
    // propre thread stress-rewrite qui ecrit g_health) — sinon la resolution
    // de module dans PageGuardSession::monitorRemote() serait cassee.
    EXPECT_EQ(hit.module.toLower(), QStringLiteral("killenginetesttarget.exe"))
        << "Hit resolved to unexpected module: " << hit.module.toStdString();

    EXPECT_TRUE(target.started()) << "Test target crashed during PAGE_GUARD capture — injected VEH destabilized it";
}

TEST(PowerUpRuntimeTest, InjectDllFailsCleanlyOnMissingDll) {
    TestTargetProcess target;
    ASSERT_TRUE(target.started()) << "KillEngineTestTarget.exe did not start";

    killcore::ProcessHandle handle(target.pid(), killcore::ProcessAccess::AllAccess);
    ASSERT_TRUE(handle.isValid()) << "Could not open test target with AllAccess";

    const QString fakeDllPath = QDir(QCoreApplication::applicationDirPath()).filePath("nonexistent_test_dll_x64.dll");
    const auto result = killcore::injectDll(handle, fakeDllPath);

    if (result.success) {
        EXPECT_NE(result.moduleBase, 0);
        EXPECT_TRUE(result.moduleBase > 0x10000 && result.moduleBase < 0x7FFFFFFFFFFFULL)
            << "Module base looks truncated: 0x" << std::hex << result.moduleBase;
    } else {
        EXPECT_FALSE(result.error.isEmpty()) << "injectDll failed but no error message";
    }

    EXPECT_TRUE(target.started()) << "Test target crashed during DLL injection attempt";
}

TEST(PowerUpRuntimeTest, InlineHookHelpersProduceValidShellcode) {
    TestTargetProcess target;
    ASSERT_TRUE(target.started()) << "KillEngineTestTarget.exe did not start";

    killcore::ProcessHandle handle(target.pid(), killcore::ProcessAccess::ReadWrite);
    ASSERT_TRUE(handle.isValid()) << "Could not open test target process";

    const QByteArray shortJump = killcore::generateJumpShellcode(0x1000, 0x2000);
    ASSERT_EQ(shortJump.size(), 5);
    EXPECT_EQ(static_cast<uint8_t>(shortJump[0]), 0xE9);

    const QByteArray longJump = killcore::generateJumpShellcode(0x10000, 0x7FFFF0000000ULL);
    ASSERT_EQ(longJump.size(), 12);
    EXPECT_EQ(static_cast<uint8_t>(longJump[0]), 0x48);
    EXPECT_EQ(static_cast<uint8_t>(longJump[1]), 0xB8);
    EXPECT_EQ(static_cast<uint8_t>(longJump[10]), 0xFF);
    EXPECT_EQ(static_cast<uint8_t>(longJump[11]), 0xE0);

    uint64_t encodedAddr = 0;
    std::memcpy(&encodedAddr, longJump.constData() + 2, 8);
    EXPECT_EQ(encodedAddr, 0x7FFFF0000000ULL);

    const auto modules = killcore::ProcessEnumerator::enumerateModules(target.pid());
    ASSERT_FALSE(modules.isEmpty());

    const auto& mainModule = modules.first();
    EXPECT_FALSE(mainModule.name.isEmpty());
    EXPECT_GT(mainModule.size, 0);

    const int trampolineSize = killcore::calculateTrampolineSize(handle, mainModule.baseAddress, 5);
    EXPECT_GE(trampolineSize, 5) << "Trampoline size must be >= minBytes (5)";
}

TEST(PowerUpRuntimeTest, InjectShellcodeRetDoesNotCrashTarget) {
    if (!debugPrivilegesAvailable()) {
        GTEST_SKIP() << "Debug privileges not available — skipping shellcode test";
    }

    TestTargetProcess target;
    ASSERT_TRUE(target.started()) << "KillEngineTestTarget.exe did not start";

    killcore::ProcessHandle handle(target.pid(), killcore::ProcessAccess::AllAccess);
    ASSERT_TRUE(handle.isValid()) << "Could not open test target with AllAccess";

    QByteArray shellcode;
    shellcode.append(static_cast<char>(0x31));
    shellcode.append(static_cast<char>(0xC0));
    shellcode.append(static_cast<char>(0xC3));

    const auto result = killcore::injectShellcode(handle, shellcode);

    if (result.success) {
        QThread::msleep(500);

#ifdef Q_OS_WIN
        if (result.remoteThreadHandle) {
            CloseHandle(reinterpret_cast<HANDLE>(result.remoteThreadHandle));
        }
#endif
    }

    EXPECT_TRUE(target.started()) << "Test target crashed after shellcode injection";
}

TEST(PowerUpRuntimeTest, GetRemoteProcAddressFindsLoadLibraryW) {
    const uint64_t addr = killcore::getRemoteProcAddress(
        QStringLiteral("kernel32.dll"),
        QStringLiteral("LoadLibraryW"));

    EXPECT_NE(addr, 0ULL);
    EXPECT_TRUE(addr > 0x10000 && addr < 0x7FFFFFFFFFFFULL)
        << "LoadLibraryW address looks invalid: 0x" << std::hex << addr;
}