//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Fixture for end-to-end tests through a real headless session: the tests run alcedo-cli, which
// starts alcedo_main --headless and sends each command over the session socket. On failure the
// session logs are copied to ALCEDO_AUTOMATION_FAILURE_DIR, which CI uploads.

#pragma once

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStringList>
#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include "type/supported_file_type.hpp"
#include "ui/album_backend_test_fixture.hpp"

#ifndef ALCEDO_MAIN_PATH
#error "ALCEDO_MAIN_PATH must name the alcedo_main program"
#endif
#ifndef ALCEDO_CLI_PATH
#error "ALCEDO_CLI_PATH must name the alcedo-cli program"
#endif
#ifndef ALCEDO_AUTOMATION_FAILURE_DIR
#error "ALCEDO_AUTOMATION_FAILURE_DIR must name the folder for failure logs"
#endif

namespace alcedo::automation::test {

struct CliRun {
  int         exit_code = -1;
  QJsonObject json;
  QString     stderr_text;
};

inline auto CollectCiRawFiles() -> std::vector<std::filesystem::path> {
  const std::filesystem::path        root{std::string(TEST_IMG_PATH) + "/ci_rawfiles"};
  std::vector<std::filesystem::path> paths;
  if (std::filesystem::exists(root)) {
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
      if (entry.is_regular_file() && is_supported_file(entry.path())) {
        paths.push_back(entry.path());
      }
    }
  }
  std::sort(paths.begin(), paths.end());
  return paths;
}

class AutomationE2ETestBase : public ui::test::ApplicationModuleHostTestFixture {
 protected:
  void SetUp() override {
    ApplicationModuleHostTestFixture::SetUp();
    root_        = ui::test::PathToQString(temp_dir_);
    session_dir_ = QDir(root_).filePath(QStringLiteral("sessions"));
    log_path_    = QDir(root_).filePath(QStringLiteral("host.log"));
  }

  void TearDown() override {
    if (session_running_) {
      (void)Cli({"session", "stop"});
    }
    if (HasFailure()) {
      CopyFailureLogs();
    }
    ApplicationModuleHostTestFixture::TearDown();
  }

  /// Runs alcedo-cli to completion. The session runs in another process, so a blocking wait is
  /// correct here.
  auto Cli(const QStringList& arguments) -> CliRun {
    QProcess            process;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.remove(QStringLiteral("ALCEDO_SESSION"));
    process.setProcessEnvironment(environment);
    process.start(
        QStringLiteral(ALCEDO_CLI_PATH),
        QStringList{"--session-dir", session_dir_, "--session", "e2e", "--json"} + arguments);
    CliRun run;
    if (!process.waitForStarted(30000) || !process.waitForFinished(900000)) {
      ADD_FAILURE() << "alcedo-cli did not finish: " << process.errorString().toStdString();
      process.kill();
      process.waitForFinished(10000);
      return run;
    }
    run.exit_code   = process.exitCode();
    run.json        = QJsonDocument::fromJson(process.readAllStandardOutput()).object();
    run.stderr_text = QString::fromUtf8(process.readAllStandardError());
    return run;
  }

  auto HostOptions() const -> QStringList {
    return QStringList{"--settings-dir", QDir(root_).filePath(QStringLiteral("settings")),
                       "--log-file",     log_path_,
                       "--host-binary",  QStringLiteral(ALCEDO_MAIN_PATH)} +
           extra_host_options_;
  }

  /// Starts a session with a new project and returns the project path.
  auto StartWithNewProject(const QString& name) -> QString {
    const CliRun start = Cli(QStringList{"session", "start", "--headless", "--create",
                                         root_ + QStringLiteral(",") + name} +
                             HostOptions());
    EXPECT_EQ(start.exit_code, 0) << start.stderr_text.toStdString();
    session_running_ = start.exit_code == 0;
    return start.json.value("project_path").toString();
  }

  auto StartWithProject(const QString& path) -> bool {
    const CliRun start =
        Cli(QStringList{"session", "start", "--headless", "--project", path} + HostOptions());
    EXPECT_EQ(start.exit_code, 0) << start.stderr_text.toStdString();
    session_running_ = start.exit_code == 0;
    return session_running_;
  }

  void Stop() {
    const CliRun stop = Cli({"session", "stop"});
    EXPECT_EQ(stop.exit_code, 0) << stop.stderr_text.toStdString();
    session_running_ = false;
  }

  auto Call(const QString& method, const QJsonObject& params = {}) -> CliRun {
    return Cli(
        {"call", method, QString::fromUtf8(QJsonDocument(params).toJson(QJsonDocument::Compact))});
  }

  /// Calls @p method and returns its result. Adds a failure for an error.
  auto Result(const QString& method, const QJsonObject& params = {}) -> QJsonObject {
    const CliRun run = Call(method, params);
    EXPECT_EQ(run.exit_code, 0) << method.toStdString() << ": "
                                << QJsonDocument(run.json).toJson().toStdString()
                                << run.stderr_text.toStdString();
    return run.json;
  }

  /// Imports @p files, waits for the import task, and returns its final state.
  auto ImportAndWait(const std::vector<std::filesystem::path>& files) -> QJsonObject {
    QJsonArray paths;
    for (const auto& file : files) {
      paths.push_back(ui::test::PathToQString(file));
    }
    const QString task_id = Result(QStringLiteral("library.import"), QJsonObject{{"paths", paths}})
                                .value("task_id")
                                .toString();
    EXPECT_FALSE(task_id.isEmpty());
    return Result(QStringLiteral("tasks.wait"),
                  QJsonObject{{"task_id", task_id}, {"timeout_ms", 600000}});
  }

  auto ListItems() -> QJsonArray {
    return Result(QStringLiteral("library.list"), QJsonObject{{"limit", 1000}})
        .value("items")
        .toArray();
  }

  void CopyFailureLogs() const {
    const auto*   info = ::testing::UnitTest::GetInstance()->current_test_info();
    const QString target =
        QDir(QStringLiteral(ALCEDO_AUTOMATION_FAILURE_DIR))
            .filePath(QStringLiteral("%1.%2").arg(info->test_suite_name(), info->name()));
    QDir().mkpath(target);
    QStringList logs{log_path_};
    for (const QFileInfo& entry : QDir(session_dir_).entryInfoList(QDir::Files)) {
      logs.push_back(entry.absoluteFilePath());
    }
    for (const QString& log : logs) {
      const QString copy = QDir(target).filePath(QFileInfo(log).fileName());
      QFile::remove(copy);
      QFile::copy(log, copy);
    }
  }

  QString     root_;
  QString     session_dir_;
  QString     log_path_;
  /// Appended to the `session start` options, for example an editor backend.
  QStringList extra_host_options_;
  bool        session_running_ = false;
};

}  // namespace alcedo::automation::test
