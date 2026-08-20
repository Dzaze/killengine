// =============================================================================
// Tests runtime Windows pour l'arbitre de registres de debug materiels
// (core/debug/breakpoint_arbiter.h).
//
// Contexte : incident du 19-20/08/2026 (voir docs/STRATEGY_ROOM.md) — un
// breakpoint in-process laisse arme (desarmement non deterministe, corrige
// le meme jour) puis un findWhatWrites externe enchaine sans coordination
// sur la meme cible, suivi d'un crash de la cible peu apres. La cause exacte
// n'a jamais ete prouvee par dump ; ces tests ne pretendent PAS reproduire le
// crash original, ils prouvent que les protections structurelles demandees
// explicitement par l'utilisateur fonctionnent reellement en conditions
// reelles (pas juste en theorie / test unitaire pur) :
//   1. Un breakpoint in-process qui expire SANS aucun hit se desarme de
//      facon deterministe (pas "au prochain hit hypothetique").
//   2. Deux mecanismes de breakpoint materiel (in-process / externe) ne
//      peuvent jamais etre actifs simultanement sur la meme cible.
//   3. Plusieurs cycles in-process -> externe (et l'inverse) sur plusieurs
//      PID neufs fonctionnent sans regression ni fuite d'etat entre cycles.
// =============================================================================

#include <gtest/gtest.h>

#include "debug/breakpoint_arbiter.h"
#include "debug/hardware_breakpoint.h"
#include "debug/inprocess_breakpoint.h"
#include "process/process_handle.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QThread>

#include <chrono>
#include <cstdint>
#include <optional>
#include <thread>

namespace {

// Duplique volontairement (pas partage via header) le meme patron que
// tests/integration/test_power_up_runtime.cpp -- petits helpers de test,
// coherent avec le reste du depot ou ce genre de duplication ciblee est deja
// acceptee (voir ex. le calcul DR7 duplique entre hardware_breakpoint.cpp et
// inprocess_breakpoint_handler.cpp).
class TestTargetProcess {
public:
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

    bool started() const { return m_process.state() != QProcess::NotRunning; }
    uint32_t pid() const { return static_cast<uint32_t>(m_process.processId()); }

private:
    QProcess m_process;
};

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

QString inProcessHandlerPath() {
    return QDir(QCoreApplication::applicationDirPath()).filePath("KillEngineInProcessBreakpointHandler.dll");
}

} // namespace

// Reproduit precisement le piege corrige le 20/08/2026 : une capture
// in-process qui timeout SANS aucun hit doit desarmer DR7 de facon
// deterministe, pas dependre d'un hit futur hypothetique qui n'arrivera
// jamais si personne n'ecrit a l'adresse pendant la fenetre. Cible SANS
// stress rewrite (rien n'ecrit g_health), fenetre courte : le zero-hit est
// garanti, pas suppose.
TEST(BreakpointArbiterRuntimeTest, InProcessTimeoutWithoutHitDisarmsDeterministically) {
    const QString handlerPath = inProcessHandlerPath();
    ASSERT_TRUE(QFile::exists(handlerPath)) << "KillEngineInProcessBreakpointHandler.dll not found next to test binary";

    TestTargetProcess target(/*stressRewrite=*/false);
    ASSERT_TRUE(target.started()) << "KillEngineTestTarget.exe did not start";

    const auto address = readTestTargetHealthAddress(target.pid());
    ASSERT_TRUE(address.has_value()) << "Could not read g_health address from test target marker file";

    killcore::ProcessHandle handle(target.pid(), killcore::ProcessAccess::AllAccess);
    ASSERT_TRUE(handle.isValid()) << "Could not open test target with AllAccess (required for injection)";

    killcore::InProcessBreakpointConfig config;
    config.address = *address;
    config.size = sizeof(int32_t);
    config.captureWrites = true;
    config.timeoutMs = 500; // court, borne, aucune ecriture attendue dans cette fenetre
    config.maxHits = 5;
    config.injectedHandlerPath = handlerPath;

    killcore::InProcessBreakpointSession session;
    const auto result = session.monitor(handle, config);

    EXPECT_TRUE(result.success) << "monitor() should still report success on a clean timeout: " << result.error.toStdString();
    EXPECT_TRUE(result.timedOut);
    EXPECT_TRUE(result.hits.isEmpty()) << "Test invalid: something wrote g_health during the window, zero-hit path not exercised";

    // Preuve directe du desarmement deterministe : l'arbitre doit etre
    // revenu a Idle pour ce PID (release() n'a ete appele qu'avec
    // disarmConfirmed=true si state->disarmed a reellement ete observe).
    const auto snapshot = killcore::HwBreakpointArbiter::instance().snapshot(target.pid());
    EXPECT_EQ(snapshot.owner, killcore::HwBreakpointOwner::None)
        << "Arbiter still shows an owner for this PID after a supposedly clean timeout disarm";
    EXPECT_FALSE(killcore::HwBreakpointArbiter::instance().isPoisoned(target.pid()))
        << "PID marked Error -- disarm confirmation timed out, DR7 may still be armed in the target";

    EXPECT_TRUE(target.started()) << "Test target crashed during a zero-hit in-process capture";
}

// Coeur de la protection demandee explicitement par l'utilisateur : deux
// mecanismes de breakpoint materiel (in-process / externe) ne doivent JAMAIS
// pouvoir etre actifs simultanement sur la meme cible -- refus propre plutot
// que le risque de conflit sur DR0-DR7 constate le 19-20/08/2026.
TEST(BreakpointArbiterRuntimeTest, ConcurrentInProcessAndExternalIsRefused) {
    if (!debugPrivilegesAvailable()) {
        GTEST_SKIP() << "Debug privileges not available -- skipping";
    }
    const QString handlerPath = inProcessHandlerPath();
    ASSERT_TRUE(QFile::exists(handlerPath)) << "KillEngineInProcessBreakpointHandler.dll not found next to test binary";

    // Stress rewrite volontaire ici : garde la capture in-process "Active"
    // assez longtemps (elle continue de capturer des hits) pour laisser une
    // vraie fenetre pendant laquelle tenter l'acquisition externe.
    TestTargetProcess target(/*stressRewrite=*/true);
    ASSERT_TRUE(target.started()) << "KillEngineTestTarget.exe did not start";

    const auto address = readTestTargetHealthAddress(target.pid());
    ASSERT_TRUE(address.has_value()) << "Could not read g_health address from test target marker file";

    killcore::ProcessHandle handle(target.pid(), killcore::ProcessAccess::AllAccess);
    ASSERT_TRUE(handle.isValid()) << "Could not open test target with AllAccess (required for injection)";

    killcore::InProcessBreakpointConfig config;
    config.address = *address;
    config.size = sizeof(int32_t);
    config.captureWrites = true;
    config.timeoutMs = 2000;
    config.maxHits = 200; // volontairement haut : reste actif toute la fenetre de test
    config.injectedHandlerPath = handlerPath;

    killcore::InProcessBreakpointSession session;
    session.startAsync(handle, config);

    // Laisse le temps a l'installation reelle de se terminer (injection +
    // VEH + armement) avant de tenter le conflit -- pas juste "Arming".
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    ASSERT_TRUE(session.isMonitoring()) << "In-process capture ended before the concurrency window -- test setup invalid";

    const auto midSnapshot = killcore::HwBreakpointArbiter::instance().snapshot(target.pid());
    ASSERT_EQ(midSnapshot.owner, killcore::HwBreakpointOwner::InProcess)
        << "Arbiter does not show the in-process session as owner mid-capture -- test setup invalid";

    // Tentative de findWhatWrites externe pendant que l'in-process est
    // encore actif -- doit etre refusee proprement (0 hit, pas de crash, pas
    // de corruption d'etat), pas juste "par chance ne pas crasher cette
    // fois".
    const auto externalHits = killcore::findWhatWrites(
        target.pid(), *address, killcore::BreakpointSize::DWord, /*timeoutMs=*/500, /*maxHits=*/5);
    EXPECT_TRUE(externalHits.isEmpty())
        << "External findWhatWrites should have been refused by the arbiter while in-process owns this PID";

    // L'arbitre doit toujours montrer l'in-process comme proprietaire -- la
    // tentative externe refusee ne doit avoir rien modifie.
    const auto afterSnapshot = killcore::HwBreakpointArbiter::instance().snapshot(target.pid());
    EXPECT_EQ(afterSnapshot.owner, killcore::HwBreakpointOwner::InProcess)
        << "Arbiter ownership changed after a refused external attempt -- arbitration state corrupted";

    session.stop();
    EXPECT_TRUE(target.started()) << "Test target crashed during the concurrent-attempt scenario";

    const auto finalSnapshot = killcore::HwBreakpointArbiter::instance().snapshot(target.pid());
    EXPECT_EQ(finalSnapshot.owner, killcore::HwBreakpointOwner::None)
        << "Arbiter still shows an owner after stop() -- cleanup incomplete";
}

// Plusieurs cycles in-process -> externe sur plusieurs PID neufs : verifie
// qu'aucun etat ne fuit d'un cycle/PID a l'autre (arbitre remis a Idle a
// chaque fois, chaque nouvelle cible fonctionne normalement).
TEST(BreakpointArbiterRuntimeTest, SequentialCyclesAcrossFreshPidsInProcessThenExternal) {
    if (!debugPrivilegesAvailable()) {
        GTEST_SKIP() << "Debug privileges not available -- skipping";
    }
    const QString handlerPath = inProcessHandlerPath();
    ASSERT_TRUE(QFile::exists(handlerPath)) << "KillEngineInProcessBreakpointHandler.dll not found next to test binary";

    // Chaque cycle utilise un PID FRAIS (nouvelle instance de la cible) --
    // pas plusieurs captures in-process sur le MEME PID, qui se heurterait
    // par construction a la limitation v1 deja documentee et acceptee
    // ("une seule capture/freeze par cible et par lancement de KillEngine",
    // voir le docblock de core/debug/inprocess_breakpoint.h) : ce n'est pas
    // le comportement que ce test cherche a verifier.
    for (int pidCycle = 0; pidCycle < 3; ++pidCycle) {
        SCOPED_TRACE(testing::Message() << "pidCycle=" << pidCycle);

        TestTargetProcess target(/*stressRewrite=*/true);
        ASSERT_TRUE(target.started()) << "KillEngineTestTarget.exe did not start";

        const auto address = readTestTargetHealthAddress(target.pid());
        ASSERT_TRUE(address.has_value()) << "Could not read g_health address from test target marker file";

        killcore::ProcessHandle handle(target.pid(), killcore::ProcessAccess::AllAccess);
        ASSERT_TRUE(handle.isValid()) << "Could not open test target with AllAccess";

        // In-process : capture bornee. Ne PAS exiger de hit ici -- la cible
        // de stress rewrite cree sa thread d'ecriture AVANT l'injection
        // (donc jamais armee, limitation v1 documentee et acceptee dans
        // core/debug/inprocess_breakpoint.h : seules la thread appelante et
        // les threads creees APRES l'injection sont armees). Ce test verifie
        // le cycle de vie/l'arbitrage, pas la fidelite de capture (deja
        // couverte par BreakpointArbiterRuntimeTest.
        // InProcessTimeoutWithoutHitDisarmsDeterministically et par
        // PowerUpRuntimeTest.PageGuardCapturesRemoteStressRewrite pour le
        // mecanisme PAGE_GUARD qui n'a pas cette limitation).
        killcore::InProcessBreakpointConfig ipConfig;
        ipConfig.address = *address;
        ipConfig.size = sizeof(int32_t);
        ipConfig.captureWrites = true;
        ipConfig.timeoutMs = 1000;
        ipConfig.maxHits = 5;
        ipConfig.injectedHandlerPath = handlerPath;

        killcore::InProcessBreakpointSession ipSession;
        const auto ipResult = ipSession.monitor(handle, ipConfig);
        EXPECT_TRUE(ipResult.success) << ipResult.error.toStdString();

        EXPECT_EQ(killcore::HwBreakpointArbiter::instance().snapshot(target.pid()).owner,
                  killcore::HwBreakpointOwner::None)
            << "Arbiter not back to Idle after in-process cycle";

        // Externe, immediatement apres : doit fonctionner normalement
        // maintenant que l'in-process a relache proprement.
        const auto extHits = killcore::findWhatWrites(
            target.pid(), *address, killcore::BreakpointSize::DWord, /*timeoutMs=*/1000, /*maxHits=*/5);
        EXPECT_FALSE(extHits.isEmpty()) << "External findWhatWrites found nothing right after a clean in-process release";

        EXPECT_EQ(killcore::HwBreakpointArbiter::instance().snapshot(target.pid()).owner,
                  killcore::HwBreakpointOwner::None)
            << "Arbiter not back to Idle after external cycle";

        EXPECT_TRUE(target.started()) << "Test target crashed mid-cycle";
    }
}

// Sens inverse : externe -> arret -> in-process, sur plusieurs PID neufs.
TEST(BreakpointArbiterRuntimeTest, SequentialCyclesAcrossFreshPidsExternalThenInProcess) {
    if (!debugPrivilegesAvailable()) {
        GTEST_SKIP() << "Debug privileges not available -- skipping";
    }
    const QString handlerPath = inProcessHandlerPath();
    ASSERT_TRUE(QFile::exists(handlerPath)) << "KillEngineInProcessBreakpointHandler.dll not found next to test binary";

    for (int pidCycle = 0; pidCycle < 2; ++pidCycle) {
        SCOPED_TRACE(testing::Message() << "pidCycle=" << pidCycle);

        TestTargetProcess target(/*stressRewrite=*/true);
        ASSERT_TRUE(target.started()) << "KillEngineTestTarget.exe did not start";

        const auto address = readTestTargetHealthAddress(target.pid());
        ASSERT_TRUE(address.has_value()) << "Could not read g_health address from test target marker file";

        const auto extHits = killcore::findWhatWrites(
            target.pid(), *address, killcore::BreakpointSize::DWord, /*timeoutMs=*/1000, /*maxHits=*/5);
        EXPECT_FALSE(extHits.isEmpty()) << "External findWhatWrites found nothing under stress rewrite";

        EXPECT_EQ(killcore::HwBreakpointArbiter::instance().snapshot(target.pid()).owner,
                  killcore::HwBreakpointOwner::None)
            << "Arbiter not back to Idle after external cycle";

        // Piege TROUVE et root-cause le 20/08/2026 (voir l'entree dediee dans
        // docs/STRATEGY_ROOM.md pour le detail complet de la bissection) :
        // juste apres un cycle externe complet (DebugActiveProcess ->
        // DebugActiveProcessStop), la tentative d'injection qui suit peut
        // echouer avec error=5 (ACCES REFUSE). Root-cause via un reproducteur
        // Win32 pur, independant de tout code KillEngine : PAS un bug
        // KillEngine — un EDR (Microsoft Defender for Endpoint, present sur
        // la machine de dev) traite "attache debugger -> detache -> alloue +
        // cree un thread distant dans la meme cible" comme une heuristique
        // d'injection de code, meme pour un usage legitime. Confirme non lie
        // a nos flags d'acces (PROCESS_ALL_ACCESS litteral echoue aussi), non
        // lie a un delai, et toujours present protection temps reel Windows
        // Defender desactivee (donc un AUTRE composant, l'ATP/EDR, distinct
        // du toggle temps reel classique). `injectDll` (core/inject/
        // dll_injector.cpp) ajoute desormais un message actionnable
        // (accessDeniedHint) sur ERROR_ACCESS_DENIED pointant vers cette
        // cause plutot que de laisser un code Win32 nu.
        killcore::InProcessBreakpointResult ipResult;
        for (int attempt = 0; attempt < 3; ++attempt) {
            SCOPED_TRACE(testing::Message() << "injectionAttempt=" << attempt);
            killcore::ProcessHandle handle(target.pid(), killcore::ProcessAccess::AllAccess);
            ASSERT_TRUE(handle.isValid()) << "Could not open test target with AllAccess";

            killcore::InProcessBreakpointConfig ipConfig;
            ipConfig.address = *address;
            ipConfig.size = sizeof(int32_t);
            ipConfig.captureWrites = true;
            ipConfig.timeoutMs = 1000;
            ipConfig.maxHits = 5;
            ipConfig.injectedHandlerPath = handlerPath;

            killcore::InProcessBreakpointSession ipSession;
            ipResult = ipSession.monitor(handle, ipConfig);
            if (ipResult.success) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(200 * (attempt + 1)));
        }
        if (!ipResult.success && ipResult.error.contains("antivirus/EDR", Qt::CaseInsensitive)) {
            // Signature EDR reconnue (message actionnable ajoute a
            // dll_injector.cpp) -- environnement-dependant, pas un bug
            // KillEngine. Skip documente plutot qu'echec dur ou skip
            // silencieux : le message explique precisement pourquoi.
            GTEST_SKIP() << "Injection bloquee par un antivirus/EDR (signature reconnue), pas un bug "
                            "KillEngine -- voir docs/STRATEGY_ROOM.md 20/08/2026. Erreur : "
                         << ipResult.error.toStdString();
        }
        // Toute AUTRE erreur (message different de la signature EDR connue)
        // reste un echec dur -- ce serait un vrai probleme non identifie.
        EXPECT_TRUE(ipResult.success) << ipResult.error.toStdString();
        // Pas d'assertion sur ipResult.hits -- meme limitation documentee que
        // dans SequentialCyclesAcrossFreshPidsInProcessThenExternal ci-dessus.

        EXPECT_EQ(killcore::HwBreakpointArbiter::instance().snapshot(target.pid()).owner,
                  killcore::HwBreakpointOwner::None)
            << "Arbiter not back to Idle after in-process cycle";

        EXPECT_TRUE(target.started()) << "Test target crashed mid-cycle";
    }
}
