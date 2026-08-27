// =============================================================================
// Test runtime du scanner de chaines de pointeurs (core/pointer/pointer_scanner.cpp)
//
// Objectif : reproduire en isolation, sur KillEngineTestTarget.exe, une limite
// trouvee en direct pendant PHASE 162/163 (voir docs/PHASE_TRACKER.md) :
// scanForPointerChains echoue de facon reproductible a retrouver une chaine
// pourtant triviale (profondeur 1, base module statique) vers un objet
// alloue dynamiquement (Player), alors que la valeur du pointeur statique
// qui y mene est verifiee correcte par lecture directe.
// =============================================================================

#include <gtest/gtest.h>

#include "process/process_handle.h"
#include "memory/memory_reader.h"
#include "pointer/pointer_scanner.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QThread>

#include <cstdint>
#include <optional>

namespace {

class TestTargetProcess {
public:
    // autoReallocPlayerCount > 0 : reproduit sans clic UI le scenario "Player
    // realloue au moins une fois" (voir KILLENGINE_TEST_TARGET_AUTO_REALLOC_PLAYER
    // dans tests/memory_targets/test_target_main.cpp) -- ajoute specifiquement
    // pour ce fichier de test, pour distinguer un vrai bug du scanner d'un
    // artefact specifique a l'allocation initiale du process.
    explicit TestTargetProcess(int autoReallocPlayerCount = 0) {
        const QString targetPath = QDir(QCoreApplication::applicationDirPath()).filePath("KillEngineTestTarget.exe");
        m_process.setProgram(targetPath);
        if (autoReallocPlayerCount > 0) {
            QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
            env.insert("KILLENGINE_TEST_TARGET_AUTO_REALLOC_PLAYER", QString::number(autoReallocPlayerCount));
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

// Lit l'adresse statique de &g_player exposee par KillEngineTestTarget.exe
// dans son fichier marqueur (voir tests/memory_targets/test_target_main.cpp,
// champ g_player_ptr_static -- adresse du POINTEUR lui-meme, stable d'un
// lancement a l'autre, PAS l'adresse du Player alloue sur le tas derriere).
std::optional<uint64_t> readPlayerPointerStaticAddress(uint32_t expectedPid) {
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
        } else if (parts[0] == "g_player_ptr_static") {
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

} // namespace

// Cas simple, controle : le scanner doit retrouver une chaine profondeur 1
// vers l'adresse du POINTEUR statique lui-meme (&g_player), en cherchant
// qui pointe vers son emplacement -- sert de test de non-regression sur le
// mecanisme de base (region scanning + module matching), independant du
// probleme specifique cible par le test suivant.
TEST(PointerScannerRuntime, FindsChainToStaticPointerLocationItself) {
    TestTargetProcess target;
    ASSERT_TRUE(target.started()) << "KillEngineTestTarget.exe did not start";

    killcore::ProcessHandle handle(target.pid(), killcore::ProcessAccess::ReadWrite);
    ASSERT_TRUE(handle.isValid()) << "Could not open test target process";

    const auto playerPtrAddress = readPlayerPointerStaticAddress(target.pid());
    ASSERT_TRUE(playerPtrAddress.has_value()) << "Could not read g_player_ptr_static from marker file";

    killcore::PointerScanOptions options;
    options.maxDepth = 3;
    options.maxResults = 5;
    options.onlyModuleBase = true;

    const auto result = killcore::scanForPointerChains(handle, *playerPtrAddress, options);
    EXPECT_TRUE(result.success) << result.errorMessage.toStdString();
}

// Cas cible : la valeur ACTUELLEMENT stockee dans le pointeur statique
// &g_player (l'adresse heap du Player alloue) doit etre retrouvable en un
// seul niveau de dereferencement, puisque &g_player la contient directement
// (offset 0). C'est exactement le scenario "promouvoir en Trainer via
// pointer chain" (PHASE 162/163) : on connait l'adresse heap d'une stat
// (ex: Player.health), on cherche la chaine qui y mene depuis une base
// statique. Verifie en direct (session manuelle) : ce test echoue avant
// correctif -- scanForPointerChains renvoie 0 chaine alors que
// MemoryReader confirme que &g_player contient bien exactement cette valeur.
TEST(PointerScannerRuntime, FindsChainToCurrentHeapValueOfStaticPointer) {
    TestTargetProcess target;
    ASSERT_TRUE(target.started()) << "KillEngineTestTarget.exe did not start";

    killcore::ProcessHandle handle(target.pid(), killcore::ProcessAccess::ReadWrite);
    ASSERT_TRUE(handle.isValid()) << "Could not open test target process";

    const auto playerPtrAddress = readPlayerPointerStaticAddress(target.pid());
    ASSERT_TRUE(playerPtrAddress.has_value()) << "Could not read g_player_ptr_static from marker file";

    killcore::MemoryReader reader(handle);
    const auto read = reader.read(*playerPtrAddress, sizeof(uint64_t));
    ASSERT_TRUE(read.success || read.partial);
    ASSERT_EQ(read.bytesRead, sizeof(uint64_t)) << "Could not read the current Player heap pointer value";

    uint64_t heapAddress = 0;
    std::memcpy(&heapAddress, read.data.constData(), sizeof(uint64_t));
    ASSERT_NE(heapAddress, 0u) << "g_player should be allocated at startup (see test_target_main.cpp)";

    killcore::PointerScanOptions options;
    options.maxDepth = 1;
    options.maxResults = 5;
    options.onlyModuleBase = true;

    const auto result = killcore::scanForPointerChains(handle, heapAddress, options);
    ASSERT_TRUE(result.success) << result.errorMessage.toStdString();
    EXPECT_FALSE(result.chains.isEmpty())
        << "Expected a depth-1 chain from a static module base to the Player heap address "
           "(the static pointer at 0x" << QString::number(*playerPtrAddress, 16).toStdString()
        << " directly holds 0x" << QString::number(heapAddress, 16).toStdString() << " at offset 0)";

    if (!result.chains.isEmpty()) {
        const auto& chain = result.chains.first();
        killcore::PointerChain resolveChain = chain;
        const auto resolved = killcore::resolvePointerChain(handle, resolveChain);
        EXPECT_TRUE(resolved.success) << resolved.errorMessage.toStdString();
        EXPECT_EQ(resolved.finalAddress, heapAddress);
    }
}

// Meme scenario que le test precedent, mais sur un Player realloue une fois
// avant le scan (KILLENGINE_TEST_TARGET_AUTO_REALLOC_PLAYER=1, sans clic UI).
// Hypothese testee : en session manuelle (PHASE 162/163), tous les echecs
// observes en direct l'etaient TOUJOURS apres au moins un clic sur
// "Reallocate Player" -- jamais sur l'allocation initiale du process. Si ce
// test echoue alors que FindsChainToCurrentHeapValueOfStaticPointer (meme
// scan, allocation initiale) passe, ca confirme que le probleme est
// specifique a une adresse issue d'un cycle delete/new (heap a fragmentation
// reduite potentiellement different pour un bloc reutilise), pas un bug
// general de l'algorithme de scan.
TEST(PointerScannerRuntime, FindsChainToHeapValueAfterOneReallocation) {
    TestTargetProcess target(/*autoReallocPlayerCount=*/1);
    ASSERT_TRUE(target.started()) << "KillEngineTestTarget.exe did not start";

    killcore::ProcessHandle handle(target.pid(), killcore::ProcessAccess::ReadWrite);
    ASSERT_TRUE(handle.isValid()) << "Could not open test target process";

    const auto playerPtrAddress = readPlayerPointerStaticAddress(target.pid());
    ASSERT_TRUE(playerPtrAddress.has_value()) << "Could not read g_player_ptr_static from marker file";

    killcore::MemoryReader reader(handle);
    const auto read = reader.read(*playerPtrAddress, sizeof(uint64_t));
    ASSERT_TRUE(read.success || read.partial);
    ASSERT_EQ(read.bytesRead, sizeof(uint64_t)) << "Could not read the current (reallocated) Player heap pointer value";

    uint64_t heapAddress = 0;
    std::memcpy(&heapAddress, read.data.constData(), sizeof(uint64_t));
    ASSERT_NE(heapAddress, 0u) << "g_player should still be allocated after a delete/new cycle";

    killcore::PointerScanOptions options;
    options.maxDepth = 1;
    options.maxResults = 5;
    options.onlyModuleBase = true;

    const auto result = killcore::scanForPointerChains(handle, heapAddress, options);
    ASSERT_TRUE(result.success) << result.errorMessage.toStdString();
    EXPECT_FALSE(result.chains.isEmpty())
        << "Expected a depth-1 chain to the REALLOCATED Player heap address "
           "(the static pointer at 0x" << QString::number(*playerPtrAddress, 16).toStdString()
        << " directly holds 0x" << QString::number(heapAddress, 16).toStdString()
        << " at offset 0, same as the initial-allocation test, but after one delete/new cycle)";
}
