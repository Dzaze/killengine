#include <gtest/gtest.h>

#include "process/process_handle.h"
#include "memory/memory_reader.h"
#include "memory/memory_writer.h"
#include "scanner/scan_engine.h"
#include "scanner/scan_types.h"

#include <QCoreApplication>
#include <QDir>
#include <QProcess>
#include <QThread>

#include <cstdint>
#include <memory>
#include <vector>

namespace {

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

QByteArray int32Bytes(int32_t value) {
    QByteArray bytes;
    bytes.append(reinterpret_cast<const char*>(&value), sizeof(value));
    return bytes;
}

QByteArray floatBytes(float value) {
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

std::optional<uint64_t> findPlayerMoneyAddress(
    const killcore::ScanResult& scan,
    const killcore::MemoryReader& reader) {
    const QByteArray expectedPlayerPrefix =
        int32Bytes(100)
        + int32Bytes(50)
        + int32Bytes(41250)
        + floatBytes(1.0f);

    for (const auto& match : scan.matches) {
        if (match.type != killcore::ValueType::Int32 || match.address < 8) {
            continue;
        }

        const auto read = reader.read(match.address - 8, static_cast<size_t>(expectedPlayerPrefix.size()));
        if ((read.success || read.partial)
            && read.bytesRead == static_cast<size_t>(expectedPlayerPrefix.size())
            && read.data == expectedPlayerPrefix) {
            return match.address;
        }
    }
    return std::nullopt;
}

} // namespace

TEST(IntegrationMemoryScanTest, ExactScanFindsKnownTestTargetValue) {
    TestTargetProcess target;
    ASSERT_TRUE(target.started()) << "KillEngineTestTarget.exe did not start.";

    killcore::ProcessHandle handle(target.pid(), killcore::ProcessAccess::ReadOnly);
    ASSERT_TRUE(handle.isValid()) << "Could not open KillEngineTestTarget process.";

    const killcore::ScanResult scan = scanInt32(handle, 41250);

    ASSERT_TRUE(scan.success) << scan.errorMessage.toStdString();
    EXPECT_FALSE(scan.cancelled);
    EXPECT_GT(scan.bytesScanned, 0U);
    EXPECT_GT(scan.matchesFound, 0U);
    EXPECT_FALSE(scan.matches.isEmpty());
}

TEST(IntegrationMemoryScanTest, ExactWorkflowWritesVerifiesAndRollsBackKnownValue) {
    TestTargetProcess target;
    ASSERT_TRUE(target.started()) << "KillEngineTestTarget.exe did not start.";

    killcore::ProcessHandle handle(target.pid(), killcore::ProcessAccess::ReadWrite);
    ASSERT_TRUE(handle.isValid()) << "Could not open KillEngineTestTarget process.";

    const killcore::ScanResult initialScan = scanInt32(handle, 41250);
    ASSERT_TRUE(initialScan.success) << initialScan.errorMessage.toStdString();
    ASSERT_FALSE(initialScan.matches.isEmpty());

    killcore::MemoryReader reader(handle);
    const auto playerMoneyAddress = findPlayerMoneyAddress(initialScan, reader);
    ASSERT_TRUE(playerMoneyAddress.has_value()) << "Could not identify Player.money candidate.";

    killcore::MemoryWriter writer(handle);
    const QByteArray targetBytes = int32Bytes(42000);
    const auto write = writer.write(*playerMoneyAddress, targetBytes, true);
    ASSERT_TRUE(write.success) << write.errorMessage.toStdString();
    ASSERT_TRUE(write.verified) << write.errorMessage.toStdString();

    std::vector<uint64_t> survivors;
    for (const auto& match : initialScan.matches) {
        const auto read = reader.read(match.address, sizeof(int32_t));
        if ((read.success || read.partial)
            && read.bytesRead == sizeof(int32_t)
            && read.data == targetBytes) {
            survivors.push_back(match.address);
        }
    }

    ASSERT_FALSE(survivors.empty());
    EXPECT_NE(std::find(survivors.begin(), survivors.end(), *playerMoneyAddress), survivors.end());

    const auto rollback = writer.write(*playerMoneyAddress, write.previousValue, true);
    ASSERT_TRUE(rollback.success) << rollback.errorMessage.toStdString();
    ASSERT_TRUE(rollback.verified) << rollback.errorMessage.toStdString();

    const auto restored = reader.read(*playerMoneyAddress, sizeof(int32_t));
    ASSERT_TRUE(restored.success || restored.partial) << restored.errorMessage.toStdString();
    ASSERT_EQ(restored.bytesRead, sizeof(int32_t));
    EXPECT_EQ(restored.data, int32Bytes(41250));
}
