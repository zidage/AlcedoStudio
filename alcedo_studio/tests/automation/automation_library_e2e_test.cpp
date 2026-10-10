//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// End-to-end library workflow through a real headless session: the tests run alcedo-cli, which
// starts alcedo_main --headless and sends each command over the session socket. The CI preset
// runs these tests (label ci_automation_flow). On failure the session logs are copied to
// ALCEDO_AUTOMATION_FAILURE_DIR, which CI uploads.

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSet>
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

namespace alcedo::automation {
namespace {

struct CliRun {
  int         exit_code = -1;
  QJsonObject json;
  QString     stderr_text;
};

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

auto FileNames(const std::vector<std::filesystem::path>& paths) -> QSet<QString> {
  QSet<QString> names;
  for (const auto& path : paths) {
    names.insert(QString::fromStdWString(path.filename().wstring()));
  }
  return names;
}

class AutomationLibraryE2ETest : public ui::test::ApplicationModuleHostTestFixture {
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
    return {"--settings-dir", QDir(root_).filePath(QStringLiteral("settings")),
            "--log-file",     log_path_,
            "--host-binary",  QStringLiteral(ALCEDO_MAIN_PATH)};
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

  QString root_;
  QString session_dir_;
  QString log_path_;
  bool    session_running_ = false;
};

TEST_F(AutomationLibraryE2ETest, ImportCiRawFilesListsEveryImportedFile) {
  const auto raw_files = CollectCiRawFiles();
  ASSERT_FALSE(raw_files.empty()) << "the CI RAW files are missing under " << TEST_IMG_PATH;
  ASSERT_FALSE(StartWithNewProject(QStringLiteral("import_project")).isEmpty());

  const QJsonObject task = ImportAndWait(raw_files);
  EXPECT_EQ(task.value("state").toString(), QStringLiteral("succeeded"));
  const QJsonObject counts = task.value("counts").toObject();
  EXPECT_EQ(counts.value("failed").toInt(), 0);
  EXPECT_EQ(counts.value("completed").toInt(), static_cast<int>(raw_files.size()));

  const QJsonObject page = Result(QStringLiteral("library.list"), QJsonObject{{"limit", 1000}});
  EXPECT_EQ(page.value("total").toInt(), static_cast<int>(raw_files.size()));
  const QJsonArray items = page.value("items").toArray();
  ASSERT_EQ(items.size(), static_cast<qsizetype>(raw_files.size()));
  QSet<QString> listed;
  double        previous_id = 0;
  for (const QJsonValue& value : items) {
    const QJsonObject item = value.toObject();
    listed.insert(item.value("file_name").toString());
    EXPECT_GT(item.value("element_id").toDouble(), previous_id) << "element id order";
    previous_id = item.value("element_id").toDouble();
    EXPECT_GT(item.value("image_id").toDouble(), 0);
  }
  EXPECT_EQ(listed, FileNames(raw_files));

  const QJsonArray folders = Result(QStringLiteral("library.folders")).value("folders").toArray();
  ASSERT_FALSE(folders.isEmpty());
  EXPECT_EQ(folders.first().toObject().value("depth").toInt(), 0);
  const QJsonObject root_page = Result(
      QStringLiteral("library.list"),
      QJsonObject{{"folder_id", folders.first().toObject().value("folder_id")}, {"limit", 1000}});
  EXPECT_EQ(root_page.value("total").toInt(), static_cast<int>(raw_files.size()));
  Stop();
}

TEST_F(AutomationLibraryE2ETest, ThumbnailCommandWritesPngWithRequestedLongEdge) {
  const auto raw_files = CollectCiRawFiles();
  ASSERT_FALSE(raw_files.empty()) << "the CI RAW files are missing under " << TEST_IMG_PATH;
  ASSERT_FALSE(StartWithNewProject(QStringLiteral("thumbnail_project")).isEmpty());
  ASSERT_EQ(ImportAndWait({raw_files.front()}).value("counts").toObject().value("completed"),
            QJsonValue(1));
  const QJsonArray items = ListItems();
  ASSERT_EQ(items.size(), 1);

  const QString     out = QDir(root_).filePath(QStringLiteral("thumbs/first.png"));
  const QJsonObject thumbnail =
      Result(QStringLiteral("library.thumbnail"),
             QJsonObject{{"element_id", items.first().toObject().value("element_id")},
                         {"out", out},
                         {"long_edge", 300}});
  const QImage image(out);
  ASSERT_FALSE(image.isNull()) << out.toStdString();
  EXPECT_EQ(std::max(image.width(), image.height()), 300);
  EXPECT_EQ(thumbnail.value("width").toInt(), image.width());
  EXPECT_EQ(thumbnail.value("height").toInt(), image.height());
  Stop();
}

TEST_F(AutomationLibraryE2ETest, IdleWaitReturnsAfterPostImportPersist) {
  const auto raw_files = CollectCiRawFiles();
  ASSERT_FALSE(raw_files.empty()) << "the CI RAW files are missing under " << TEST_IMG_PATH;
  const QString project_path = StartWithNewProject(QStringLiteral("idle_project"));
  ASSERT_FALSE(project_path.isEmpty());
  QJsonArray paths;
  for (const auto& file : raw_files) {
    paths.push_back(ui::test::PathToQString(file));
  }
  ASSERT_FALSE(Result(QStringLiteral("library.import"), QJsonObject{{"paths", paths}})
                   .value("task_id")
                   .toString()
                   .isEmpty());
  EXPECT_EQ(
      Result(QStringLiteral("tasks.wait"), QJsonObject{{"idle", true}, {"timeout_ms", 600000}})
          .value("idle"),
      QJsonValue(true));
  EXPECT_EQ(Result(QStringLiteral("tasks.list")).value("idle"), QJsonValue(true));
  const QJsonArray before = ListItems();
  ASSERT_EQ(before.size(), static_cast<qsizetype>(raw_files.size()));
  Stop();

  ASSERT_TRUE(StartWithProject(project_path));
  EXPECT_EQ(ListItems(), before);
  Stop();
}

TEST_F(AutomationLibraryE2ETest, ImportOfMissingPathIsRejectedWithoutTask) {
  ASSERT_FALSE(StartWithNewProject(QStringLiteral("missing_project")).isEmpty());
  const CliRun run =
      Call(QStringLiteral("library.import"),
           QJsonObject{{"paths", QJsonArray{QDir(root_).filePath(QStringLiteral("none.arw"))}}});
  EXPECT_EQ(run.exit_code, 1);
  const QJsonObject error = run.json.value("error").toObject();
  EXPECT_EQ(error.value("code").toInt(), -32602);
  EXPECT_EQ(error.value("data").toObject().value("pointer").toString(), QStringLiteral("/paths/0"));
  EXPECT_TRUE(Result(QStringLiteral("tasks.list")).value("tasks").toArray().isEmpty());
  Stop();
}

/// The rating of each listed photo, by element id.
auto RatingsById(const QJsonArray& items) -> QMap<double, int> {
  QMap<double, int> ratings;
  for (const QJsonValue& value : items) {
    ratings.insert(value.toObject().value("element_id").toDouble(),
                   value.toObject().value("rating").toInt());
  }
  return ratings;
}

TEST_F(AutomationLibraryE2ETest, RatingPersistsAfterReopen) {
  const auto raw_files = CollectCiRawFiles();
  ASSERT_GE(raw_files.size(), 4u) << "the CI RAW files are missing under " << TEST_IMG_PATH;
  const QString project_path = StartWithNewProject(QStringLiteral("rating_project"));
  ASSERT_FALSE(project_path.isEmpty());
  ASSERT_EQ(ImportAndWait(raw_files).value("state").toString(), QStringLiteral("succeeded"));
  const QJsonArray items = ListItems();
  ASSERT_EQ(items.size(), static_cast<qsizetype>(raw_files.size()));
  const QJsonValue  first  = items.at(0).toObject().value("element_id");
  const QJsonValue  second = items.at(1).toObject().value("element_id");
  const QJsonValue  third  = items.at(2).toObject().value("element_id");
  const QJsonValue  last   = items.last().toObject().value("element_id");

  // One photo uses the direct path.
  const QJsonObject single = Result(QStringLiteral("library.rate"),
                                    QJsonObject{{"element_ids", QJsonArray{first}}, {"rating", 5}});
  EXPECT_EQ(single.value("applied_count").toInt(), 1);
  // Two photos use the batch save task.
  const QJsonObject batch =
      Result(QStringLiteral("library.rate"),
             QJsonObject{{"element_ids", QJsonArray{second, third}}, {"rating", 2}});
  const QString task_id = batch.value("task_id").toString();
  ASSERT_FALSE(task_id.isEmpty());
  EXPECT_EQ(Result(QStringLiteral("tasks.wait"), QJsonObject{{"task_id", task_id}})
                .value("state")
                .toString(),
            QStringLiteral("succeeded"));
  // Delete one photo, as the CI workflow does.
  EXPECT_EQ(Result(QStringLiteral("library.delete"),
                   QJsonObject{{"element_ids", QJsonArray{last}}, {"scope", "project"}})
                .value("deleted_ids")
                .toArray(),
            QJsonArray{last});
  EXPECT_EQ(Result(QStringLiteral("tasks.wait"), QJsonObject{{"idle", true}}).value("idle"),
            QJsonValue(true));
  const QJsonArray before = ListItems();
  ASSERT_EQ(before.size(), items.size() - 1);
  Stop();

  ASSERT_TRUE(StartWithProject(project_path));
  const QJsonArray after = ListItems();
  EXPECT_EQ(after, before);
  const QMap<double, int> ratings = RatingsById(after);
  EXPECT_EQ(ratings.value(first.toDouble()), 5);
  EXPECT_EQ(ratings.value(second.toDouble()), 2);
  EXPECT_EQ(ratings.value(third.toDouble()), 2);
  EXPECT_FALSE(ratings.contains(last.toDouble()));
  Stop();
}

TEST_F(AutomationLibraryE2ETest, RatingOutsideRangeIsRejected) {
  const auto raw_files = CollectCiRawFiles();
  ASSERT_FALSE(raw_files.empty()) << "the CI RAW files are missing under " << TEST_IMG_PATH;
  ASSERT_FALSE(StartWithNewProject(QStringLiteral("range_project")).isEmpty());
  ASSERT_EQ(ImportAndWait({raw_files.front()}).value("counts").toObject().value("completed"),
            QJsonValue(1));
  const QJsonValue element = ListItems().at(0).toObject().value("element_id");

  for (const int rating : {6, -1}) {
    const CliRun run = Call(QStringLiteral("library.rate"),
                            QJsonObject{{"element_ids", QJsonArray{element}}, {"rating", rating}});
    EXPECT_EQ(run.exit_code, 1);
    const QJsonObject error = run.json.value("error").toObject();
    EXPECT_EQ(error.value("code").toInt(), -32602);
    EXPECT_EQ(error.value("data").toObject().value("pointer").toString(),
              QStringLiteral("/rating"));
    const QString expected_range =
        rating > 5 ? QStringLiteral("maximum 5") : QStringLiteral("minimum 0");
    EXPECT_TRUE(error.value("message").toString().contains(expected_range))
        << error.value("message").toString().toStdString();
  }
  EXPECT_EQ(ListItems().at(0).toObject().value("rating").toInt(), 0);
  Stop();
}

}  // namespace
}  // namespace alcedo::automation
