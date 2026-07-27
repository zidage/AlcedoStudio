//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "utils/diagnostics/app_logging.hpp"

#include <gtest/gtest.h>

#include <QFile>
#include <QLoggingCategory>
#include <QString>
#include <QTemporaryDir>

#include <chrono>
#include <cstddef>
#include <string>

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

}  // namespace
}  // namespace alcedo::diag