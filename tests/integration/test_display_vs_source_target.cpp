// Cible synthetique "champ affiche vs champ source" (candidat POWER_UP_ROADMAP,
// voir docs/STRATEGY_ROOM.md entree "XP Solitaire enfin controlable" du
// 20/08/2026). Objectif : prouver de facon reproductible et automatisee, sur
// une cible deterministe plutot que sur un vrai jeu, le motif observe alors --
// un champ affiche recalcule a chaque tick par interpolation vers une source
// ne peut structurellement pas etre fait tenir par une ecriture externe
// directe, alors que la source, elle, tient. C'est la base de validation
// avant de coder une heuristique produit qui detecterait ce motif tout seule
// (pas encore fait ici, volontairement -- voir PHASE_TRACKER.md).

#include <gtest/gtest.h>

#include "memory/memory_reader.h"
#include "memory/memory_writer.h"
#include "process/process_handle.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QThread>

#include <cstdint>
#include <cstring>
#include <optional>

namespace {

// Meme tick que test_target_main.cpp (m_counterTimer, 50ms) et
// kCounterStepPerTick (10) -- dupliques ici en commentaire, pas en code,
// pour choisir des delais de test surs sans dependre d'un header partage.
constexpr int kCounterTickMs = 50;

class TestTargetProcess {
public:
    TestTargetProcess() {
        const QString targetPath = QDir(QCoreApplication::applicationDirPath()).filePath("KillEngineTestTarget.exe");
        m_process.setProgram(targetPath);
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

struct CounterAddresses {
    uint64_t source = 0;
    uint64_t current = 0;
    uint64_t displayed = 0;
};

// Lit les adresses reelles exposees par KillEngineTestTarget.exe dans son
// fichier marqueur (voir tests/memory_targets/test_target_main.cpp) --
// fiable contrairement a un scan par valeur, qui peut tomber sur n'importe
// quelle autre variable Qt/CRT qui vaut coincidemment la meme chose.
std::optional<CounterAddresses> readCounterAddresses(uint32_t expectedPid) {
    QFile marker(QDir::temp().filePath("killengine_test_target_addresses.txt"));
    if (!marker.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return std::nullopt;
    }
    const QString content = QString::fromUtf8(marker.readAll());
    marker.close();

    uint32_t markerPid = 0;
    CounterAddresses addresses;
    for (const QString& line : content.split('\n', Qt::SkipEmptyParts)) {
        const auto parts = line.split('=');
        if (parts.size() != 2) continue;
        bool ok = false;
        if (parts[0] == "pid") {
            markerPid = parts[1].toUInt();
        } else if (parts[0] == "g_counterSource") {
            addresses.source = parts[1].toULongLong(&ok, 16);
        } else if (parts[0] == "g_counterCurrent") {
            addresses.current = parts[1].toULongLong(&ok, 16);
        } else if (parts[0] == "g_counterDisplayed") {
            addresses.displayed = parts[1].toULongLong(&ok, 16);
        }
    }

    if (markerPid != expectedPid || addresses.source == 0 || addresses.current == 0 || addresses.displayed == 0) {
        return std::nullopt;
    }
    return addresses;
}

std::optional<int32_t> readInt32(const killcore::MemoryReader& reader, uint64_t address) {
    const auto read = reader.read(address, sizeof(int32_t));
    if (!(read.success || read.partial) || read.bytesRead != sizeof(int32_t)) {
        return std::nullopt;
    }
    int32_t value = 0;
    std::memcpy(&value, read.data.constData(), sizeof(int32_t));
    return value;
}

bool writeInt32(killcore::MemoryWriter& writer, uint64_t address, int32_t value) {
    QByteArray bytes;
    bytes.append(reinterpret_cast<const char*>(&value), sizeof(value));
    const auto write = writer.write(address, bytes, true);
    return write.success && write.verified;
}

} // namespace

TEST(DisplayVsSourceTargetTest, WriteToSourceHolds) {
    TestTargetProcess target;
    ASSERT_TRUE(target.started()) << "KillEngineTestTarget.exe did not start.";

    const auto addresses = readCounterAddresses(target.pid());
    ASSERT_TRUE(addresses.has_value()) << "Could not read counter addresses from marker file.";

    killcore::ProcessHandle handle(target.pid(), killcore::ProcessAccess::ReadWrite);
    ASSERT_TRUE(handle.isValid()) << "Could not open KillEngineTestTarget process.";
    killcore::MemoryReader reader(handle);
    killcore::MemoryWriter writer(handle);

    constexpr int32_t kSentinel = 424242;
    ASSERT_TRUE(writeInt32(writer, addresses->source, kSentinel)) << "Write to g_counterSource failed.";

    // Plusieurs ticks (50ms chacun) : rien ne doit recalculer g_counterSource,
    // contrairement a g_counterDisplayed.
    QThread::msleep(kCounterTickMs * 4);

    const auto readBack = readInt32(reader, addresses->source);
    ASSERT_TRUE(readBack.has_value()) << "Could not read back g_counterSource.";
    EXPECT_EQ(*readBack, kSentinel) << "Write to the SOURCE field did not hold -- something else is rewriting it.";
}

TEST(DisplayVsSourceTargetTest, WriteToDisplayedDoesNotHold) {
    TestTargetProcess target;
    ASSERT_TRUE(target.started()) << "KillEngineTestTarget.exe did not start.";

    const auto addresses = readCounterAddresses(target.pid());
    ASSERT_TRUE(addresses.has_value()) << "Could not read counter addresses from marker file.";

    killcore::ProcessHandle handle(target.pid(), killcore::ProcessAccess::ReadWrite);
    ASSERT_TRUE(handle.isValid()) << "Could not open KillEngineTestTarget process.";
    killcore::MemoryReader reader(handle);
    killcore::MemoryWriter writer(handle);

    constexpr int32_t kSentinel = 999999;
    ASSERT_TRUE(writeInt32(writer, addresses->displayed, kSentinel)) << "Write to g_counterDisplayed failed.";

    // Un seul tick suffit largement (50ms) : le prochain timeout recalcule
    // g_counterDisplayed depuis g_counterCurrent, quelle que soit la valeur
    // qu'on vient d'y ecrire. On attend un peu plus pour eviter tout effet
    // de bord de timing (scheduler, charge machine).
    QThread::msleep(kCounterTickMs * 3);

    const auto readBack = readInt32(reader, addresses->displayed);
    ASSERT_TRUE(readBack.has_value()) << "Could not read back g_counterDisplayed.";
    EXPECT_NE(*readBack, kSentinel)
        << "Write to the DISPLAYED field held -- it should have been overwritten by the next interpolation tick "
        << "(this would mean the synthetic target's tick logic is broken, not that the heuristic problem doesn't exist).";
}

TEST(DisplayVsSourceTargetTest, SourceChangePropagatesToDisplayedGradually) {
    TestTargetProcess target;
    ASSERT_TRUE(target.started()) << "KillEngineTestTarget.exe did not start.";

    const auto addresses = readCounterAddresses(target.pid());
    ASSERT_TRUE(addresses.has_value()) << "Could not read counter addresses from marker file.";

    killcore::ProcessHandle handle(target.pid(), killcore::ProcessAccess::ReadWrite);
    ASSERT_TRUE(handle.isValid()) << "Could not open KillEngineTestTarget process.";
    killcore::MemoryReader reader(handle);
    killcore::MemoryWriter writer(handle);

    const auto initialDisplayed = readInt32(reader, addresses->displayed);
    ASSERT_TRUE(initialDisplayed.has_value());

    // Petit delta (kCounterStepPerTick=10/tick @ 50ms -> ~10 ticks pour
    // rattraper 100, 500ms) : assez pour observer une vraie interpolation
    // sans rendre le test lent.
    constexpr int32_t kDelta = 100;
    ASSERT_TRUE(writeInt32(writer, addresses->source, *initialDisplayed + kDelta));

    QThread::msleep(kCounterTickMs * 3);
    const auto midDisplayed = readInt32(reader, addresses->displayed);
    ASSERT_TRUE(midDisplayed.has_value());
    EXPECT_GT(*midDisplayed, *initialDisplayed)
        << "Displayed should already be catching up toward the new source after a few ticks.";
    EXPECT_LT(*midDisplayed, *initialDisplayed + kDelta)
        << "Displayed should not have fully caught up yet this early (interpolation should still be gradual).";

    QThread::msleep(kCounterTickMs * 15);
    const auto finalDisplayed = readInt32(reader, addresses->displayed);
    ASSERT_TRUE(finalDisplayed.has_value());
    EXPECT_EQ(*finalDisplayed, *initialDisplayed + kDelta)
        << "Displayed should have fully caught up to the new source after enough ticks.";
}
