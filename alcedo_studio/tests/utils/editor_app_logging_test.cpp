//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <chrono>
#include <cstddef>
#include <string>
#include <utility>

#include "utils/diagnostics/app_logging.hpp"
#include "utils/logging/log.hpp"

namespace alcedo::diag {
namespace {

/// Owns one temporary log directory and initializes production logging exactly
/// once per process. Each test resets the diagnostic counters and installs a
/// deterministic filter rule set so assertions do not depend on test order.
class EditorAppLoggingTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    temp_dir_ = std::make_unique<QTemporaryDir>();
    initialized_path_ = InitializeApplicationLogging(temp_dir_->path());
  }

  static void TearDownTestSuite() {
    ShutdownApplicationLogging();
    temp_dir_.reset();
  }

  void SetUp() override {
    ResetDiagnosticCountersForTesting();
    // Restore the production default policy before every test.
    QLoggingCategory::setFilterRules(QStringLiteral(
        "*.debug=false\n"
        "qt.*.info=false\n"
        "alcedo.*.info=true\n"
        "alcedo.editor.present.info=false\n"
        "alcedo.*.warning=true\n"));
  }

  /// Count "[EditorPresent]" log lines appended after the current file offset.
  /// Captures the offset before emitting so accumulation across tests does not
  /// pollute the count.
  auto CountNewPresentLines() -> std::size_t {
    QFile file(CurrentLogFilePath());
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return 0;
    const auto start = file.size();
    file.seek(start);
    std::size_t count = 0;
    while (!file.atEnd()) {
      const QByteArray line = file.readLine();
      if (line.contains("[EditorPresent]")) ++count;
    }
    return count;
  }

  static std::unique_ptr<QTemporaryDir> temp_dir_;
  static QString                        initialized_path_;
};

std::unique_ptr<QTemporaryDir> EditorAppLoggingTest::temp_dir_;
QString                        EditorAppLoggingTest::initialized_path_;

/// R0/R1 regression: a default application run must not write per-frame
/// presentation info to the log. The present category is disabled at info level
/// by default and per-frame detail lives at debug level (also off by default).
TEST_F(EditorAppLoggingTest, DefaultLoggingWritesNoPerFramePresentationInfo) {
  ASSERT_FALSE(initialized_path_.isEmpty()) << "logging failed to initialize";

  // Simulate several per-frame presentation emissions (debug + info severity).
  for (int i = 0; i < 5; ++i) {
    qCDebug(editorPresentLog, "[EditorPresent] frame %d request=%llu", i, 0ULL);
    qCInfo(editorPresentLog, "[EditorPresent] frame summary %d", i);
  }

  const auto counters = DiagnosticCountersSnapshot();
  EXPECT_EQ(counters.present_messages_written, 0u)
      << "default rules must suppress every per-frame presentation message";
  EXPECT_EQ(CountNewPresentLines(), 0u)
      << "the log file must contain zero per-frame presentation info lines by default";
}

/// Enabling the present debug category restores the per-frame diagnostic
/// detail without changing the default policy.
TEST_F(EditorAppLoggingTest, EnablingPresentDebugCategoryRestoresPerFrameDetail) {
  ASSERT_FALSE(initialized_path_.isEmpty());
  QLoggingCategory::setFilterRules(QStringLiteral(
      "*.debug=false\n"
      "alcedo.editor.present.debug=true\n"
      "alcedo.editor.present.info=true\n"
      "alcedo.*.warning=true\n"));

  qCDebug(editorPresentLog, "[EditorPresent] frame request=%llu", 42ULL);
  qCInfo(editorPresentLog, "[EditorPresent] frame summary");

  const auto counters = DiagnosticCountersSnapshot();
  EXPECT_GE(counters.present_messages_written, 1u)
      << "enabling the present category must route per-frame detail to the handler";
}

/// Info/debug writes must not flush the log file once per message. The handler
/// buffers them and flushes only on a bounded batch threshold or shutdown.
TEST_F(EditorAppLoggingTest, InfoAndDebugLoggingDoesNotFlushOncePerFrame) {
  ASSERT_FALSE(initialized_path_.isEmpty());
  // alcedo.app info is enabled by the default policy, so these reach the handler.
  for (int i = 0; i < 32; ++i) {
    qCInfo(appLog, "buffered editor lifecycle message %d", i);
  }

  const auto counters = DiagnosticCountersSnapshot();
  EXPECT_EQ(counters.immediate_flushes, 0u)
      << "info/debug writes must never trigger an immediate per-line flush";
  EXPECT_EQ(counters.info_debug_messages_written, 32u);
}

/// Warning, critical, and fatal severities flush immediately so terminal errors
/// stay visible without waiting for a batch or shutdown.
TEST_F(EditorAppLoggingTest, WarningAndCriticalFlushImmediately) {
  ASSERT_FALSE(initialized_path_.isEmpty());
  ResetDiagnosticCountersForTesting();

  qCWarning(editorPresentLog, "[EditorPresent] handshake failed");
  qCCritical(appLog, "editor backend failed critically");

  const auto counters = DiagnosticCountersSnapshot();
  EXPECT_EQ(counters.immediate_flushes, 2u)
      << "warning and critical messages must each flush immediately";
  EXPECT_GE(counters.present_messages_written, 1u);
}

/// A message of the Qt-free interface (utils/logging/log.hpp) reaches the application log file with
/// its category name and the source location of the ALCEDO_LOG_* call.
TEST_F(EditorAppLoggingTest, QtFreeInterfaceMessageReachesLogFileWithCategoryAndSourceLocation) {
  ASSERT_FALSE(initialized_path_.isEmpty());
  const int expected_line = __LINE__ + 1;
  ALCEDO_LOG_WARN(Gpu) << "core-interface-marker requested_bytes=" << 4096;

  // Warnings flush immediately, so the line is in the file now.
  QFile file(CurrentLogFilePath());
  ASSERT_TRUE(file.open(QIODevice::ReadOnly | QIODevice::Text));
  QByteArray marker_line;
  while (!file.atEnd()) {
    const QByteArray line = file.readLine();
    if (line.contains("core-interface-marker")) {
      marker_line = line;
    }
  }
  ASSERT_FALSE(marker_line.isEmpty()) << "the core message is missing from the log file";
  EXPECT_TRUE(marker_line.contains("[WARN]")) << marker_line.constData();
  EXPECT_TRUE(marker_line.contains("[alcedo.gpu]")) << marker_line.constData();
  EXPECT_TRUE(marker_line.contains("core-interface-marker requested_bytes=4096"))
      << marker_line.constData();
  const QByteArray location =
      QByteArrayLiteral("editor_app_logging_test.cpp:") + QByteArray::number(expected_line) + ')';
  EXPECT_TRUE(marker_line.contains(location)) << marker_line.constData();
}

/// The Qt rules decide the enabled levels of the Qt-free interface, also when they change after
/// initialization.
TEST_F(EditorAppLoggingTest, QtRulesSetTheEnabledLevelsOfTheQtFreeInterface) {
  ASSERT_FALSE(initialized_path_.isEmpty());
  EXPECT_FALSE(log::IsEnabled(log::Level::Debug, log::Category::RenderRoi));
  EXPECT_TRUE(log::IsEnabled(log::Level::Info, log::Category::GpuPool));
  EXPECT_FALSE(log::IsEnabled(log::Level::Info, log::Category::EditorPresent));

  QLoggingCategory::setFilterRules(QStringLiteral(
      "*.debug=false\n"
      "alcedo.render.roi.debug=true\n"
      "alcedo.gpu.pool.info=false\n"));

  EXPECT_TRUE(log::IsEnabled(log::Level::Debug, log::Category::RenderRoi));
  EXPECT_FALSE(log::IsEnabled(log::Level::Debug, log::Category::Render));
  EXPECT_FALSE(log::IsEnabled(log::Level::Info, log::Category::GpuPool));
  EXPECT_TRUE(log::IsEnabled(log::Level::Warn, log::Category::GpuPool));
}

/// Retention of the log directory (#823). Each test initializes logging in its own directory and
/// shuts it down. A session is `alcedo_<time>_<pid>.log` and `alcedo_<time>_<pid>_mind.log`.
class ApplicationLogRetentionTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ShutdownApplicationLogging();
    ASSERT_TRUE(temp_dir_.isValid());
  }

  void        TearDown() override { ShutdownApplicationLogging(); }

  auto        Dir() const -> QDir { return QDir(temp_dir_.path()); }

  /// The stem `<time>_<pid>` of old session @p index; a higher index is a newer session. The
  /// process IDs differ from the ID of the test process.
  static auto SessionStem(int index) -> QString {
    const qint64 pid = QCoreApplication::applicationPid() + 1 + index;
    return QStringLiteral("20260101_10%1_%2").arg(index, 4, 10, QLatin1Char('0')).arg(pid);
  }

  void WriteFile(const QString& name, qint64 size) const {
    QFile file(Dir().filePath(name));
    ASSERT_TRUE(file.open(QIODevice::WriteOnly)) << name.toStdString();
    ASSERT_TRUE(file.resize(size)) << name.toStdString();
  }

  /// Writes the application and the sidecar file of each session in [0, @p count).
  void WriteSessions(int count, qint64 bytes_per_file) const {
    for (int index = 0; index < count; ++index) {
      WriteFile(QStringLiteral("alcedo_%1.log").arg(SessionStem(index)), bytes_per_file);
      WriteFile(QStringLiteral("alcedo_%1_mind.log").arg(SessionStem(index)), bytes_per_file);
    }
  }

  auto SessionFilesExist(int index) const -> std::pair<bool, bool> {
    return {Dir().exists(QStringLiteral("alcedo_%1.log").arg(SessionStem(index))),
            Dir().exists(QStringLiteral("alcedo_%1_mind.log").arg(SessionStem(index)))};
  }

  QTemporaryDir temp_dir_;
};

/// 25 old sessions and other files: initialization keeps the newest 20 sessions, opens the new
/// session, and keeps the other files.
TEST_F(ApplicationLogRetentionTest, KeepsTheNewestTwentySessionsAndOtherFiles) {
  WriteSessions(25, 16);
  WriteFile(QStringLiteral("notes.txt"), 16);
  WriteFile(QStringLiteral("alcedo_custom.log"), 16);

  const QString path = InitializeApplicationLogging(temp_dir_.path());
  ASSERT_FALSE(path.isEmpty());

  for (int index = 0; index < 5; ++index) {
    EXPECT_EQ(SessionFilesExist(index), std::make_pair(false, false)) << index;
  }
  for (int index = 5; index < 25; ++index) {
    EXPECT_EQ(SessionFilesExist(index), std::make_pair(true, true)) << index;
  }
  EXPECT_TRUE(Dir().exists(QStringLiteral("notes.txt")));
  EXPECT_TRUE(Dir().exists(QStringLiteral("alcedo_custom.log")));
  EXPECT_TRUE(QFileInfo::exists(path));
  // 20 old sessions with two files each, the new log file, and alcedo_custom.log.
  EXPECT_EQ(Dir().entryList(QStringList{QStringLiteral("alcedo_*.log")}, QDir::Files).size(),
            2 * kRetainedLogSessionCount + 2);
}

/// Sessions of 30 MiB: initialization removes the oldest until the old sessions hold at most
/// 100 MiB.
TEST_F(ApplicationLogRetentionTest, RemovesTheOldestSessionsAboveTheSizeLimit) {
  constexpr qint64 kBytesPerFile = qint64{15} * 1024 * 1024;  // Two files: 30 MiB a session.
  WriteSessions(5, kBytesPerFile);

  ASSERT_FALSE(InitializeApplicationLogging(temp_dir_.path()).isEmpty());

  EXPECT_EQ(SessionFilesExist(0), std::make_pair(false, false));
  EXPECT_EQ(SessionFilesExist(1), std::make_pair(false, false));
  for (int index = 2; index < 5; ++index) {
    EXPECT_EQ(SessionFilesExist(index), std::make_pair(true, true)) << index;
  }
  qint64 kept_bytes = 0;
  for (const QFileInfo& file :
       Dir().entryInfoList(QStringList{QStringLiteral("alcedo_2026*.log")}, QDir::Files)) {
    kept_bytes += file.size();
  }
  EXPECT_LE(kept_bytes, kRetainedLogSessionBytes);
}

/// The explicit file of the headless host (`--log-file`) removes no file.
TEST_F(ApplicationLogRetentionTest, ExplicitLogFileRemovesNoSession) {
  WriteSessions(25, 16);

  ASSERT_FALSE(
      InitializeApplicationLoggingToFile(Dir().filePath(QStringLiteral("headless.log"))).isEmpty());

  for (int index = 0; index < 25; ++index) {
    EXPECT_EQ(SessionFilesExist(index), std::make_pair(true, true)) << index;
  }
}

/// The sidecar log of the session is the application log name with `_mind` before `.log`.
TEST_F(ApplicationLogRetentionTest, CompanionLogFileHasTheStemOfTheApplicationLog) {
  EXPECT_TRUE(CompanionLogFilePath(QStringLiteral("mind")).isEmpty());

  const QString path = InitializeApplicationLogging(temp_dir_.path());
  ASSERT_FALSE(path.isEmpty());
  const QFileInfo application_log(path);
  EXPECT_EQ(CompanionLogFilePath(QStringLiteral("mind")),
            application_log.dir().filePath(application_log.completeBaseName() +
                                           QStringLiteral("_mind.log")));

  ShutdownApplicationLogging();
  EXPECT_TRUE(CompanionLogFilePath(QStringLiteral("mind")).isEmpty());
}

#ifdef _WIN32
/// A file that another handle holds open stays, and its failure is one warning line in
/// alcedo.app with the OS error. The next initialization removes it.
TEST_F(ApplicationLogRetentionTest, FileThatCannotBeRemovedStaysAndWritesOneWarning) {
  WriteSessions(kRetainedLogSessionCount + 1, 16);
  const QString locked_name = QStringLiteral("alcedo_%1.log").arg(SessionStem(0));
  // QFile opens without FILE_SHARE_DELETE on Windows, so DeleteFile fails while it is open.
  QFile         locked(Dir().filePath(locked_name));
  ASSERT_TRUE(locked.open(QIODevice::ReadOnly));

  const QString path = InitializeApplicationLogging(temp_dir_.path());
  ASSERT_FALSE(path.isEmpty());
  EXPECT_EQ(SessionFilesExist(0), std::make_pair(true, false));

  QFile log_file(path);
  ASSERT_TRUE(log_file.open(QIODevice::ReadOnly | QIODevice::Text));
  int warning_lines = 0;
  while (!log_file.atEnd()) {
    const QByteArray line = log_file.readLine();
    if (line.contains("logging.retention.remove_failed")) {
      ++warning_lines;
      EXPECT_TRUE(line.contains("[WARN]")) << line.constData();
      EXPECT_TRUE(line.contains("[alcedo.app]")) << line.constData();
      EXPECT_TRUE(line.contains(locked_name.toUtf8())) << line.constData();
      EXPECT_TRUE(line.contains("error=")) << line.constData();
    }
  }
  EXPECT_EQ(warning_lines, 1);
  log_file.close();
  ShutdownApplicationLogging();

  locked.close();
  ASSERT_FALSE(InitializeApplicationLogging(temp_dir_.path()).isEmpty());
  EXPECT_EQ(SessionFilesExist(0), std::make_pair(false, false));
}
#endif

}  // namespace
}  // namespace alcedo::diag