//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "automation/automation_session_file.hpp"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QProcess>
#include <QTemporaryDir>
#include <QTimeZone>
#include <utility>
#include <vector>

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

void WriteBytes(const QString& path, const QByteArray& bytes) {
  QFile output(path);
  ASSERT_TRUE(output.open(QIODevice::WriteOnly));
  ASSERT_EQ(output.write(bytes), bytes.size());
}

/// The JSON text of a session record that names a process which does not run, with @p key set
/// to @p value, or removed when @p value is undefined.
auto DeadSessionRecordWith(const QString& name, const char* key, const QJsonValue& value)
    -> QByteArray {
  QJsonObject record{{"protocol_version", 1},
                     {"session_name", name},
                     {"socket_name", "alcedo-automation-1-" + name},
                     {"pid", static_cast<double>(ExitedProcessId())},
                     {"host_mode", "headless"},
                     {"project_path", "D:/Photos/trip.alcd"},
                     {"started_at", "2026-10-08T09:00:00Z"}};
  if (value.isUndefined()) {
    record.remove(QLatin1String(key));
  } else {
    record.insert(QLatin1String(key), value);
  }
  return QJsonDocument(record).toJson(QJsonDocument::Compact);
}

TEST(AutomationSessionFileTest, UnreadableFileIsListedWithErrorAndIsNotStale) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  WriteBytes(dir.filePath("broken.json"), "{\"pid\": \"x\"}");

  const auto entries = ReadAutomationSessionFiles(dir.path());

  ASSERT_EQ(entries.size(), 1U);
  EXPECT_FALSE(entries[0].file.has_value());
  EXPECT_FALSE(entries[0].stale);
  EXPECT_FALSE(entries[0].read_error.isEmpty());
}

TEST(AutomationSessionFileTest, FilesThatAreNotSessionRecordsAreRejectedAndAreNeverStale) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const std::vector<std::pair<QString, QByteArray>> files = {
      {"user-settings", R"({"theme":"dark","valuable":"keep"})"},
      {"malformed", R"({"session_name": "malformed", )"},
      {"array", R"([{"session_name":"array","pid":1}])"},
      {"no-socket", DeadSessionRecordWith("no-socket", "socket_name", QJsonValue::Undefined)},
      {"no-project", DeadSessionRecordWith("no-project", "project_path", QJsonValue::Undefined)},
      {"no-start", DeadSessionRecordWith("no-start", "started_at", QJsonValue::Undefined)},
      {"zero-pid", DeadSessionRecordWith("zero-pid", "pid", 0)},
      {"fraction-pid", DeadSessionRecordWith("fraction-pid", "pid", 12.5)},
      {"text-pid", DeadSessionRecordWith("text-pid", "pid", "12")},
      {"zero-protocol", DeadSessionRecordWith("zero-protocol", "protocol_version", 0)},
      {"bad-mode", DeadSessionRecordWith("bad-mode", "host_mode", "daemon")},
      {"bad-start", DeadSessionRecordWith("bad-start", "started_at", "yesterday")},
      {"bad-name", DeadSessionRecordWith("bad-name", "session_name", "a/b")},
      {"other-name", DeadSessionRecordWith("other-name", "session_name", "settings")},
  };
  for (const auto& [name, bytes] : files) {
    WriteBytes(dir.filePath(name + ".json"), bytes);
  }

  const auto entries = ReadAutomationSessionFiles(dir.path());

  ASSERT_EQ(entries.size(), files.size());
  for (const AutomationSessionFileEntry& entry : entries) {
    SCOPED_TRACE(entry.path.toStdString());
    EXPECT_FALSE(entry.file.has_value());
    EXPECT_FALSE(entry.stale);
    EXPECT_FALSE(entry.read_error.isEmpty());
  }
}

TEST(AutomationSessionFileTest, ValidRecordOfDeadProcessIsStale) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  WriteBytes(dir.filePath("complete.json"),
             DeadSessionRecordWith("complete", "host_mode", QStringLiteral("gui")));

  const auto entries = ReadAutomationSessionFiles(dir.path());

  ASSERT_EQ(entries.size(), 1U);
  ASSERT_TRUE(entries[0].file.has_value()) << entries[0].read_error.toStdString();
  EXPECT_EQ(entries[0].file->host_mode, QStringLiteral("gui"));
  EXPECT_TRUE(entries[0].stale);
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
