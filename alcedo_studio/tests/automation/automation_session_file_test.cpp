//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "automation/automation_session_file.hpp"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QTimeZone>
#include <utility>

namespace alcedo::automation {
namespace {

auto MakeSessionFile(QString name, qint64 pid) -> AutomationSessionFile {
  AutomationSessionFile file;
  file.protocol_version = 1;
  file.session_name     = std::move(name);
  file.socket_name      = QStringLiteral("alcedo-automation-%1").arg(pid);
  file.pid              = pid;
  file.host_mode        = QStringLiteral("headless");
  file.project_path     = QStringLiteral("D:/Photos/trip.alcd");
  file.started_at       = QDateTime(QDate(2026, 10, 8), QTime(9, 0, 0), QTimeZone::UTC);
  return file;
}

/// The id of a process that ran and exited.
auto ExitedProcessId() -> qint64 {
  QProcess process;
  process.start(QCoreApplication::applicationFilePath(), {QStringLiteral("--gtest_list_tests")});
  EXPECT_TRUE(process.waitForStarted(10000));
  const qint64 pid = process.processId();
  EXPECT_TRUE(process.waitForFinished(30000));
  return pid;
}

TEST(AutomationSessionFileTest, WrittenFileReadsBackWithSameFields) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const AutomationSessionFile written =
      MakeSessionFile(QStringLiteral("default"), QCoreApplication::applicationPid());

  QString error;
  ASSERT_TRUE(WriteAutomationSessionFile(dir.filePath("sessions"), written, &error))
      << error.toStdString();
  const auto read = ReadAutomationSessionFile(
      AutomationSessionFilePath(dir.filePath("sessions"), QStringLiteral("default")), &error);

  ASSERT_TRUE(read.has_value()) << error.toStdString();
  EXPECT_EQ(read->protocol_version, written.protocol_version);
  EXPECT_EQ(read->session_name, written.session_name);
  EXPECT_EQ(read->socket_name, written.socket_name);
  EXPECT_EQ(read->pid, written.pid);
  EXPECT_EQ(read->host_mode, written.host_mode);
  EXPECT_EQ(read->project_path, written.project_path);
  EXPECT_EQ(read->started_at, written.started_at);
}

TEST(AutomationSessionFileTest, DeadProcessFileIsReportedStale) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const qint64 dead_pid = ExitedProcessId();
  ASSERT_GT(dead_pid, 0);
  ASSERT_TRUE(WriteAutomationSessionFile(dir.path(), MakeSessionFile("dead", dead_pid)));
  ASSERT_TRUE(WriteAutomationSessionFile(
      dir.path(), MakeSessionFile("live", QCoreApplication::applicationPid())));

  const auto entries = ReadAutomationSessionFiles(dir.path());

  ASSERT_EQ(entries.size(), 2U);
  EXPECT_EQ(entries[0].file->session_name, QStringLiteral("dead"));
  EXPECT_TRUE(entries[0].stale);
  EXPECT_EQ(entries[1].file->session_name, QStringLiteral("live"));
  EXPECT_FALSE(entries[1].stale);
}

TEST(AutomationSessionFileTest, UnreadableFileIsListedAsStaleWithError) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  QFile broken(dir.filePath("broken.json"));
  ASSERT_TRUE(broken.open(QIODevice::WriteOnly));
  broken.write("{\"pid\": \"x\"}");
  broken.close();

  const auto entries = ReadAutomationSessionFiles(dir.path());

  ASSERT_EQ(entries.size(), 1U);
  EXPECT_FALSE(entries[0].file.has_value());
  EXPECT_TRUE(entries[0].stale);
  EXPECT_FALSE(entries[0].read_error.isEmpty());
}

TEST(AutomationSessionFileTest, NameWithPathSeparatorIsRejected) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());

  QString error;
  EXPECT_FALSE(WriteAutomationSessionFile(dir.path(), MakeSessionFile("../escape", 1), &error));
  EXPECT_FALSE(error.isEmpty());
  EXPECT_FALSE(IsValidAutomationSessionName(QStringLiteral("a/b")));
  EXPECT_TRUE(IsValidAutomationSessionName(QStringLiteral("ci-run_2.main")));
}

TEST(AutomationSessionFileTest, RemoveDeletesTheFile) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  ASSERT_TRUE(WriteAutomationSessionFile(dir.path(), MakeSessionFile("default", 1)));

  EXPECT_TRUE(RemoveAutomationSessionFile(dir.path(), QStringLiteral("default")));
  EXPECT_FALSE(QFile::exists(AutomationSessionFilePath(dir.path(), QStringLiteral("default"))));
  EXPECT_TRUE(RemoveAutomationSessionFile(dir.path(), QStringLiteral("default")));
}

}  // namespace
}  // namespace alcedo::automation
