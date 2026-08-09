#include <gtest/gtest.h>
#include "logging/logger.h"
#include <QCoreApplication>
#include <QTemporaryDir>

TEST(LoggerTest, Singleton) {
    auto& a = killcore::Logger::instance();
    auto& b = killcore::Logger::instance();
    EXPECT_EQ(&a, &b);
}

TEST(LoggerTest, InitAndLog) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());

    auto& logger = killcore::Logger::instance();
    logger.init(tmpDir.path());

    logger.setLevel(killcore::LogLevel::Trace);
    EXPECT_EQ(logger.level(), killcore::LogLevel::Trace);

    KE_LOG_INFO() << "Test message";

    EXPECT_FALSE(logger.logFilePath().isEmpty());
    EXPECT_TRUE(QFile::exists(logger.logFilePath()));
}

TEST(LoggerTest, LevelFiltering) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());

    auto& logger = killcore::Logger::instance();
    logger.init(tmpDir.path());
    logger.setLevel(killcore::LogLevel::Warn);

    // These should be filtered out (below Warn)
    KE_LOG_TRACE() << "trace";
    KE_LOG_DEBUG() << "debug";
    KE_LOG_INFO() << "info";

    // These should be logged
    KE_LOG_WARN() << "warning";
    KE_LOG_ERROR() << "error";

    // Verify file has content
    QFile f(logger.logFilePath());
    ASSERT_TRUE(f.open(QIODevice::ReadOnly | QIODevice::Text));
    QString content = QString::fromUtf8(f.readAll());
    f.close();

    // Warning and Error should be in the file
    EXPECT_TRUE(content.contains("WARN"));
    EXPECT_TRUE(content.contains("ERROR"));
}