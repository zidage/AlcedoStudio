//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/automation/headless_host.hpp"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QSettings>
#include <QTemporaryDir>

#ifndef ALCEDO_MAIN_PATH
#error "ALCEDO_MAIN_PATH must name the alcedo_main program"
#endif

namespace alcedo::automation {
namespace {

struct HostRun {
  int     exit_code = -1;
  QString stderr_text;
};

auto RunHost(const QStringList& arguments) -> HostRun {
  QProcess process;
  process.start(QStringLiteral(ALCEDO_MAIN_PATH), arguments);
  HostRun run;
  if (!process.waitForStarted(30000) || !process.waitForFinished(300000)) {
    ADD_FAILURE() << "alcedo_main did not finish: " << process.errorString().toStdString();
    process.kill();
    process.waitForFinished(10000);
    return run;
  }
  run.exit_code   = process.exitCode();
  run.stderr_text = QString::fromUtf8(process.readAllStandardError());
  return run;
}

TEST(HeadlessHostTest, SettingsDirRedirectsDefaultQSettings) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  QCoreApplication::setOrganizationName(QStringLiteral("Alcedo"));
  QCoreApplication::setApplicationName(QStringLiteral("Alcedo"));

  ApplyHeadlessSettingsDirectory(dir.path());
  QString settings_path;
  {
    QSettings settings;
    settings.setValue(QStringLiteral("automation/redirect_check"), 42);
    settings.sync();
    EXPECT_EQ(settings.format(), QSettings::IniFormat);
    settings_path = settings.fileName();
  }
  QSettings::setDefaultFormat(QSettings::NativeFormat);

  EXPECT_TRUE(QFileInfo(settings_path)
                  .absoluteFilePath()
                  .startsWith(QFileInfo(dir.path()).absoluteFilePath()))
      << settings_path.toStdString();
  ASSERT_TRUE(QFileInfo::exists(settings_path));
  const QSettings written(settings_path, QSettings::IniFormat);
  EXPECT_EQ(written.value(QStringLiteral("automation/redirect_check")).toInt(), 42);
}

TEST(HeadlessHostTest, CreateOptionTakesFolderAndName) {
  HeadlessHostOptions options;
  QString             error;

  ASSERT_TRUE(ParseHeadlessHostOptions({"--headless", "--create", "D:/Photos", "trip", "--session",
                                        "ci", "--viewport", "1280x720", "--editor-backend=cuda"},
                                       &options, &error))
      << error.toStdString();
  EXPECT_EQ(options.create_folder, QStringLiteral("D:/Photos"));
  EXPECT_EQ(options.create_name, QStringLiteral("trip"));
  EXPECT_EQ(options.session_name, QStringLiteral("ci"));
  EXPECT_EQ(options.viewport_width, 1280);
  EXPECT_EQ(options.viewport_height, 720);
  ASSERT_TRUE(options.editor_backend.has_value());
  EXPECT_EQ(*options.editor_backend, editor_rhi::EditorBackend::Cuda);
}

TEST(HeadlessHostTest, ProjectAndCreateTogetherAreRejected) {
  HeadlessHostOptions options;
  QString             error;

  EXPECT_FALSE(ParseHeadlessHostOptions({"--project", "a.alcd", "--create", "D:/Photos", "trip"},
                                        &options, &error));
  EXPECT_FALSE(error.isEmpty());
  HeadlessHostOptions bad_viewport;
  EXPECT_FALSE(ParseHeadlessHostOptions({"--project", "a.alcd", "--viewport", "wide"},
                                        &bad_viewport, &error));
}

TEST(HeadlessHostTest, UnknownOptionExitsWithUsageCode) {
  const HostRun run = RunHost({"--headless", "--project", "a.alcd", "--bogus"});

  EXPECT_EQ(run.exit_code, static_cast<int>(HeadlessHostExitCode::UsageError));
  EXPECT_TRUE(run.stderr_text.contains(QStringLiteral("--bogus")));
}

TEST(HeadlessHostTest, ProjectLoadFailureExitsWithLoadCode) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString session_dir = dir.filePath(QStringLiteral("sessions"));

  const HostRun run =
      RunHost({"--headless", "--project", dir.filePath(QStringLiteral("missing.alcd")),
               "--session-dir", session_dir, "--settings-dir", dir.filePath("settings"),
               "--log-file", dir.filePath("host.log")});

  EXPECT_EQ(run.exit_code, static_cast<int>(HeadlessHostExitCode::ProjectLoadError))
      << run.stderr_text.toStdString();
  EXPECT_TRUE(run.stderr_text.contains(QStringLiteral("project")));
  EXPECT_TRUE(QDir(session_dir).entryList({QStringLiteral("*.json")}, QDir::Files).isEmpty());
}

}  // namespace
}  // namespace alcedo::automation
