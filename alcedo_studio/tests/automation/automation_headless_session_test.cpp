//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Headless session tests. The first test drives the real alcedo-cli and alcedo_main --headless
// processes. The second test runs ApplicationModuleHost with the headless sink in this process
// and opens a CI RAW file through the editor session.

#include <gtest/gtest.h>

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "app/editor_session_types.hpp"
#include "automation/automation_session_file.hpp"
#include "ui/album_backend_test_fixture.hpp"
#include "ui/alcedo_main/automation/headless_frame_sink.hpp"
#include "ui/alcedo_main/automation/headless_host.hpp"

#ifndef ALCEDO_MAIN_PATH
#error "ALCEDO_MAIN_PATH must name the alcedo_main program"
#endif
#ifndef ALCEDO_CLI_PATH
#error "ALCEDO_CLI_PATH must name the alcedo-cli program"
#endif

namespace alcedo::automation {
namespace {

using ui::ApplicationModuleHost;

/// Isolated temporary directory and process setup of the module host tests.
class AutomationHeadlessSessionTest : public ui::test::ApplicationModuleHostTestFixture {};

struct CliRun {
  int         exit_code = -1;
  QJsonObject json;
  QString     stderr_text;
};

/// Runs alcedo-cli to completion. The session runs in another process, so a blocking wait is
/// correct here.
auto RunCli(const QStringList& arguments) -> CliRun {
  QProcess            process;
  QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
  environment.remove(QStringLiteral("ALCEDO_SESSION"));
  process.setProcessEnvironment(environment);
  process.start(QStringLiteral(ALCEDO_CLI_PATH), arguments);
  CliRun run;
  if (!process.waitForStarted(30000) || !process.waitForFinished(600000)) {
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

auto ReadText(const QString& path) -> QString {
  QFile file(path);
  return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
}

TEST_F(AutomationHeadlessSessionTest, StartPingStopRemovesSessionFile) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString     session_dir = dir.filePath(QStringLiteral("sessions"));
  const QString     log_path    = dir.filePath(QStringLiteral("host.log"));
  const QStringList session_options{"--session-dir", session_dir, "--session", "ci", "--json"};
  const QStringList host_options{"--settings-dir", dir.filePath(QStringLiteral("settings")),
                                 "--log-file",     log_path,
                                 "--host-binary",  QStringLiteral(ALCEDO_MAIN_PATH)};

  const CliRun      start = RunCli(
      session_options +
      QStringList{"session", "start", "--headless", "--create", dir.path() + ",au3_project"} +
      host_options);
  ASSERT_EQ(start.exit_code, 0) << start.stderr_text.toStdString();
  const QString project_path = start.json.value("project_path").toString();
  const qint64  pid          = static_cast<qint64>(start.json.value("pid").toDouble());
  EXPECT_TRUE(project_path.endsWith(QStringLiteral(".alcd"))) << project_path.toStdString();
  const QString session_file = AutomationSessionFilePath(session_dir, QStringLiteral("ci"));
  EXPECT_TRUE(QFile::exists(session_file));

  const CliRun ping = RunCli(session_options + QStringList{"call", "session.ping"});
  EXPECT_EQ(ping.exit_code, 0) << ping.stderr_text.toStdString();
  EXPECT_EQ(ping.json.value("pong"), QJsonValue(true));

  const CliRun state = RunCli(session_options + QStringList{"call", "state.get"});
  ASSERT_EQ(state.exit_code, 0) << state.stderr_text.toStdString();
  const QJsonObject project = state.json.value("project").toObject();
  EXPECT_EQ(project.value("loaded"), QJsonValue(true));
  EXPECT_EQ(project.value("entered"), QJsonValue(true));
  EXPECT_EQ(project.value("path").toString(), project_path);
  EXPECT_EQ(state.json.value("host_mode"), QJsonValue("headless"));
  EXPECT_EQ(state.json.value("control").toObject().value("held"), QJsonValue(true));

  const CliRun stop = RunCli(session_options + QStringList{"session", "stop"});
  ASSERT_EQ(stop.exit_code, 0) << stop.stderr_text.toStdString();
  EXPECT_FALSE(QFile::exists(session_file));
  EXPECT_FALSE(IsAutomationProcessRunning(pid));
  EXPECT_TRUE(
      ReadText(log_path).contains(QRegularExpression(QStringLiteral("app\\.exit code=\\s*0"))))
      << "the host log does not record exit code 0";

  // Shutdown persisted the project: a second session opens the same package.
  ASSERT_TRUE(QFile::exists(project_path));
  const CliRun restart = RunCli(
      session_options + QStringList{"session", "start", "--headless", "--project", project_path} +
      host_options);
  ASSERT_EQ(restart.exit_code, 0) << restart.stderr_text.toStdString();
  const CliRun reopened = RunCli(session_options + QStringList{"call", "state.get"});
  EXPECT_EQ(reopened.json.value("project").toObject().value("path").toString(), project_path);
  EXPECT_EQ(reopened.json.value("project").toObject().value("loaded"), QJsonValue(true));
  EXPECT_EQ(RunCli(session_options + QStringList{"session", "stop"}).exit_code, 0);
  EXPECT_FALSE(QFile::exists(session_file));
}

auto CollectCiRawFiles() -> std::vector<std::filesystem::path> {
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

template <class Predicate>
auto WaitUntil(Predicate&& predicate, std::chrono::milliseconds timeout) -> bool {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!predicate() && std::chrono::steady_clock::now() < deadline) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return predicate();
}

/// One editor backend per test process. ctest runs each case alone, so the process-wide active
/// backend never changes inside a process.
class AutomationHeadlessSessionBackendTest
    : public ui::test::ApplicationModuleHostTestFixture,
      public ::testing::WithParamInterface<editor_rhi::EditorBackend> {};

TEST_P(AutomationHeadlessSessionBackendTest, EditorOpenReachesInteractiveWithHeadlessSink) {
  const editor_rhi::EditorBackend backend = GetParam();
  if (!editor_rhi::IsBackendSupportedOnThisPlatform(backend) ||
      !editor_rhi::IsBackendAvailableInThisBuild(backend)) {
    GTEST_SKIP() << "platform unavailable: " << editor_rhi::ToString(backend)
                 << " is not supported on this platform or not built";
  }
  const auto raw_files = CollectCiRawFiles();
  ASSERT_FALSE(raw_files.empty()) << "the CI RAW files are missing under " << TEST_IMG_PATH;
  const QString backend_error = StartHeadlessEditorBackend(backend);
  ASSERT_TRUE(backend_error.isEmpty()) << backend_error.toStdString();
  ASSERT_EQ(editor_rhi::ActiveEditorBackend(), backend);

  HeadlessFrameSink     sink;
  ApplicationModuleHost host;
  host.project()->SetRuntimeAcceleratorPreference(HeadlessAcceleratorPreference(backend));
  ASSERT_TRUE(host.editor_session()->BindHeadlessPresentationSink(&sink, 1280, 720));
  ASSERT_TRUE(CreateTestProject(host));

  host.import_export()->StartImport(ui::test::PathsToQStringList({raw_files.front()}));
  ASSERT_TRUE(
      WaitUntil([&] { return !host.import_export()->ImportRunning(); }, std::chrono::minutes(2)));
  ASSERT_EQ(host.import_export()->ImportFailed(), 0);
  ASSERT_TRUE(
      WaitUntil([&] { return !host.library()->Thumbnails().isEmpty(); }, std::chrono::seconds(30)));
  const QVariantMap item     = host.library()->Thumbnails().front().toMap();
  const uint        element  = item.value("elementId").toUInt();
  const uint        image_id = item.value("imageId").toUInt();
  ASSERT_NE(element, 0U);
  ASSERT_NE(image_id, 0U);

  host.workspace_router()->OpenEditor(element, image_id);
  const bool interactive = WaitUntil(
      [&] {
        return host.editor_session_service()->state() == EditorSessionState::Interactive &&
               host.editor_session()->can_edit();
      },
      std::chrono::minutes(2));

  EXPECT_TRUE(interactive) << "state "
                           << EditorSessionStateName(host.editor_session_service()->state());
  EXPECT_GT(sink.ready_frame_count(), 0U);
  EXPECT_GT(sink.GetWidth(), 0);
  EXPECT_GT(sink.GetHeight(), 0);
  host.Shutdown();
}

INSTANTIATE_TEST_SUITE_P(EditorBackends, AutomationHeadlessSessionBackendTest,
                         ::testing::Values(editor_rhi::EditorBackend::Cuda,
                                           editor_rhi::EditorBackend::OpenCl,
                                           editor_rhi::EditorBackend::Metal),
                         [](const ::testing::TestParamInfo<editor_rhi::EditorBackend>& info) {
                           return std::string(editor_rhi::ToString(info.param));
                         });

}  // namespace
}  // namespace alcedo::automation
