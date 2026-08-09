#include <gtest/gtest.h>

#include "process/process_handle.h"
#include "scanner/scan_engine.h"
#include "scanner/scan_types.h"

#include <QCoreApplication>
#include <QDir>
#include <QProcess>
#include <QThread>

#include <cstdint>
#include <memory>

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

} // namespace

TEST(IntegrationMemoryScanTest, ExactScanFindsKnownTestTargetValue) {
    TestTargetProcess target;
    ASSERT_TRUE(target.started()) << "KillEngineTestTarget.exe did not start.";

    killcore::ProcessHandle handle(target.pid(), killcore::ProcessAccess::ReadOnly);
    ASSERT_TRUE(handle.isValid()) << "Could not open KillEngineTestTarget process.";

    killcore::ScanValue value;
    QString parseError;
    ASSERT_TRUE(killcore::parseScanValue("41250", killcore::ValueType::Int32, &value, &parseError))
        << parseError.toStdString();

    killcore::ScanOptions options;
    options.maxResults = 1000000;

    killcore::ScanEngine scanner(handle);
    const killcore::ScanResult scan = scanner.exactScan(value, options);

    ASSERT_TRUE(scan.success) << scan.errorMessage.toStdString();
    EXPECT_FALSE(scan.cancelled);
    EXPECT_GT(scan.bytesScanned, 0U);
    EXPECT_GT(scan.matchesFound, 0U);
    EXPECT_FALSE(scan.matches.isEmpty());
}
