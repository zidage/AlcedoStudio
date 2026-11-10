//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// End-to-end library workflow through a real headless session: the tests run alcedo-cli, which
// starts alcedo_main --headless and sends each command over the session socket. The CI preset
// runs these tests (label ci_automation_flow). On failure the session logs are copied to
// ALCEDO_AUTOMATION_FAILURE_DIR, which CI uploads.

#include <gtest/gtest.h>

#include <QByteArray>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QPair>
#include <QSet>
#include <QStringList>
#include <algorithm>
#include <filesystem>
#include <vector>

#include "automation_e2e_session.hpp"

namespace alcedo::automation {
namespace {

using test::CliRun;
using test::CollectCiRawFiles;

auto FileNames(const std::vector<std::filesystem::path>& paths) -> QSet<QString> {
  QSet<QString> names;
  for (const auto& path : paths) {
    names.insert(QString::fromStdWString(path.filename().wstring()));
  }
  return names;
}

class AutomationLibraryE2ETest : public test::AutomationE2ETestBase {
 protected:
  /// Runs a mark batch command and waits for its save task. Returns the final task state.
  auto MarkAndWait(const QString& method, const QJsonObject& params) -> QString {
    const QJsonObject started = Result(method, params);
    EXPECT_EQ(started.value("marked_count").toInt(), params.value("element_ids").toArray().size());
    EXPECT_EQ(started.value("on"), params.value("on"));
    const QString task_id = started.value("task_id").toString();
    EXPECT_FALSE(task_id.isEmpty()) << method.toStdString();
    return Result(QStringLiteral("tasks.wait"), QJsonObject{{"task_id", task_id}})
        .value("state")
        .toString();
  }
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

TEST_F(AutomationLibraryE2ETest, ThumbnailsCommandWritesOnePngPerElementInRequestOrder) {
  const auto raw_files = CollectCiRawFiles();
  ASSERT_GE(raw_files.size(), 2u) << "the CI RAW files are missing under " << TEST_IMG_PATH;
  ASSERT_FALSE(StartWithNewProject(QStringLiteral("thumbnails_project")).isEmpty());
  const std::vector<std::filesystem::path> imported(raw_files.begin(), raw_files.begin() + 2);
  ASSERT_EQ(ImportAndWait(imported).value("counts").toObject().value("completed"), QJsonValue(2));
  const QJsonArray listed = ListItems();
  ASSERT_EQ(listed.size(), 2);
  // The reverse of the list order: the answer keeps the request order.
  const QJsonArray  ids{listed.at(1).toObject().value("element_id"),
                       listed.at(0).toObject().value("element_id")};

  const QString     out_dir = QDir(root_).filePath(QStringLiteral("sheet_thumbs"));
  const QJsonObject result =
      Result(QStringLiteral("library.thumbnails"),
             QJsonObject{{"element_ids", ids}, {"out_dir", out_dir}, {"long_edge", 200}});
  EXPECT_EQ(result.value("written_count").toInt(), 2);
  EXPECT_EQ(result.value("failed_count").toInt(), 0);
  const QJsonArray items = result.value("items").toArray();
  ASSERT_EQ(items.size(), 2);
  for (qsizetype index = 0; index < items.size(); ++index) {
    const QJsonObject item = items.at(index).toObject();
    EXPECT_EQ(item.value("element_id"), ids.at(index));
    const QString expected_path = QDir(out_dir).filePath(
        QStringLiteral("%1.png").arg(static_cast<qint64>(ids.at(index).toDouble())));
    EXPECT_EQ(QDir::cleanPath(item.value("path").toString()), QDir::cleanPath(expected_path));
    const QImage image(expected_path);
    ASSERT_FALSE(image.isNull()) << expected_path.toStdString();
    EXPECT_EQ(std::max(image.width(), image.height()), 200);
    EXPECT_EQ(item.value("width").toInt(), image.width());
    EXPECT_EQ(item.value("height").toInt(), image.height());
  }

  const CliRun unknown =
      Call(QStringLiteral("library.thumbnails"),
           QJsonObject{{"element_ids", QJsonArray{999999}}, {"out_dir", out_dir}});
  EXPECT_EQ(unknown.exit_code, 1);
  EXPECT_EQ(unknown.json.value("error").toObject().value("code").toInt(), -32602);
  Stop();
}

// Issue #401: a thumbnail command never replaces an existing file, and reports it.
TEST_F(AutomationLibraryE2ETest, ThumbnailCommandsKeepExistingFilesAndSourceImageUnchanged) {
  const auto raw_files = CollectCiRawFiles();
  ASSERT_GE(raw_files.size(), 2u) << "the CI RAW files are missing under " << TEST_IMG_PATH;
  ASSERT_FALSE(StartWithNewProject(QStringLiteral("no_replace_project")).isEmpty());
  const std::vector<std::filesystem::path> imported(raw_files.begin(), raw_files.begin() + 2);
  ASSERT_EQ(ImportAndWait(imported).value("counts").toObject().value("completed"), QJsonValue(2));
  const QJsonArray listed = ListItems();
  ASSERT_EQ(listed.size(), 2);
  const QJsonValue first_id  = listed.at(0).toObject().value("element_id");
  const QJsonValue second_id = listed.at(1).toObject().value("element_id");

  const QByteArray user_data("USER DATA MUST SURVIVE");
  const QString    folder = QDir(root_).filePath(QStringLiteral("user_files"));
  ASSERT_TRUE(QDir().mkpath(folder));
  const auto write_user_file = [&user_data](const QString& path) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::NewOnly) &&
           file.write(user_data) == user_data.size();
  };
  const auto read_bytes = [](const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
  };
  const QString document = QDir(folder).filePath(QStringLiteral("important-document.txt"));
  const QString png_name = QDir(folder).filePath(QStringLiteral("important-document.png"));
  ASSERT_TRUE(write_user_file(document));
  ASSERT_TRUE(write_user_file(png_name));
  const QString    source_path = ui::test::PathToQString(raw_files.front());
  const QByteArray source_hash =
      QCryptographicHash::hash(read_bytes(source_path), QCryptographicHash::Sha256);
  ASSERT_FALSE(read_bytes(source_path).isEmpty());

  for (const QString& out : {document, png_name, source_path}) {
    const CliRun run = Call(QStringLiteral("library.thumbnail"),
                            QJsonObject{{"element_id", first_id}, {"out", out}, {"long_edge", 32}});
    EXPECT_EQ(run.exit_code, 1) << out.toStdString();
    const QJsonObject error = run.json.value("error").toObject();
    EXPECT_EQ(error.value("code").toInt(), -32602) << out.toStdString();
    EXPECT_EQ(error.value("data").toObject().value("pointer").toString(), QStringLiteral("/out"));
  }
  EXPECT_EQ(read_bytes(document), user_data);
  EXPECT_EQ(read_bytes(png_name), user_data);
  EXPECT_EQ(QCryptographicHash::hash(read_bytes(source_path), QCryptographicHash::Sha256),
            source_hash);

  // library.thumbnails: the existing <element_id>.png fails its item; the other is written.
  const QString out_dir = QDir(root_).filePath(QStringLiteral("batch_thumbs"));
  ASSERT_TRUE(QDir().mkpath(out_dir));
  const QString existing_path = QDir(out_dir).filePath(
      QStringLiteral("%1.png").arg(static_cast<qint64>(first_id.toDouble())));
  ASSERT_TRUE(write_user_file(existing_path));
  const QJsonObject result = Result(QStringLiteral("library.thumbnails"),
                                    QJsonObject{{"element_ids", QJsonArray{first_id, second_id}},
                                                {"out_dir", out_dir},
                                                {"long_edge", 64}});
  EXPECT_EQ(result.value("written_count").toInt(), 1);
  EXPECT_EQ(result.value("failed_count").toInt(), 1);
  const QJsonArray items = result.value("items").toArray();
  ASSERT_EQ(items.size(), 2);
  EXPECT_TRUE(items.at(0).toObject().value("error").toString().contains(QStringLiteral("exists")))
      << items.at(0).toObject().value("error").toString().toStdString();
  EXPECT_FALSE(items.at(0).toObject().contains("path"));
  EXPECT_EQ(read_bytes(existing_path), user_data);
  const QImage written(items.at(1).toObject().value("path").toString());
  ASSERT_FALSE(written.isNull());
  EXPECT_EQ(std::max(written.width(), written.height()), 64);

  // A second request finds both files and replaces neither.
  const QByteArray  written_bytes = read_bytes(items.at(1).toObject().value("path").toString());
  const QJsonObject again         = Result(QStringLiteral("library.thumbnails"),
                                           QJsonObject{{"element_ids", QJsonArray{first_id, second_id}},
                                                       {"out_dir", out_dir},
                                                       {"long_edge", 128}});
  EXPECT_EQ(again.value("written_count").toInt(), 0);
  EXPECT_EQ(again.value("failed_count").toInt(), 2);
  EXPECT_EQ(read_bytes(existing_path), user_data);
  EXPECT_EQ(read_bytes(items.at(1).toObject().value("path").toString()), written_bytes);
  Stop();
}

TEST_F(AutomationLibraryE2ETest, InfoCommandReturnsSourcePathAndCaptureValues) {
  const auto raw_files = CollectCiRawFiles();
  ASSERT_FALSE(raw_files.empty()) << "the CI RAW files are missing under " << TEST_IMG_PATH;
  ASSERT_FALSE(StartWithNewProject(QStringLiteral("info_project")).isEmpty());
  ASSERT_EQ(ImportAndWait({raw_files.front()}).value("counts").toObject().value("completed"),
            QJsonValue(1));
  const QJsonObject listed = ListItems().first().toObject();

  const QJsonArray  items =
      Result(QStringLiteral("library.info"),
             QJsonObject{{"element_ids", QJsonArray{listed.value("element_id")}}})
          .value("items")
          .toArray();
  ASSERT_EQ(items.size(), 1);
  const QJsonObject item = items.first().toObject();
  EXPECT_EQ(item.value("element_id"), listed.value("element_id"));
  EXPECT_EQ(item.value("file_name"), listed.value("file_name"));
  EXPECT_EQ(item.value("rating"), listed.value("rating"));
  EXPECT_FALSE(item.contains("metadata_error"))
      << item.value("metadata_error").toString().toStdString();
  EXPECT_EQ(std::filesystem::path(item.value("file_path").toString().toStdWString()),
            raw_files.front());
  // The CI RAW files are camera files with a full capture record.
  EXPECT_FALSE(item.value("camera_model").toString().isEmpty());
  EXPECT_GT(item.value("iso").toDouble(), 0);
  EXPECT_GT(item.value("aperture").toDouble(), 0);
  EXPECT_GT(item.value("focal_mm").toDouble(), 0);
  EXPECT_GT(item.value("shutter_seconds").toDouble(), 0);
  EXPECT_TRUE(item.value("shutter").toString().endsWith(QLatin1Char('s')));
  EXPECT_EQ(item.value("capture_time").toString().size(), 19) << "YYYY-MM-DD HH:MM:SS";
  EXPECT_GT(item.value("width").toInt(), 0);
  EXPECT_GT(item.value("height").toInt(), 0);
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

/// The `color_labels` and `delete_mark` of the library.info items, by element id.
auto MarksById(const QJsonArray& items) -> QMap<double, QPair<QJsonArray, bool>> {
  QMap<double, QPair<QJsonArray, bool>> marks;
  for (const QJsonValue& value : items) {
    const QJsonObject item = value.toObject();
    marks.insert(item.value("element_id").toDouble(),
                 {item.value("color_labels").toArray(), item.value("delete_mark").toBool()});
  }
  return marks;
}

auto ElementIds(const QJsonArray& items) -> QSet<double> {
  QSet<double> ids;
  for (const QJsonValue& value : items) {
    ids.insert(value.toObject().value("element_id").toDouble());
  }
  return ids;
}

TEST_F(AutomationLibraryE2ETest, ColorLabelsAndDeleteMarkChangeAndFilterTheList) {
  const auto raw_files = CollectCiRawFiles();
  ASSERT_GE(raw_files.size(), 3u) << "the CI RAW files are missing under " << TEST_IMG_PATH;
  ASSERT_FALSE(StartWithNewProject(QStringLiteral("marks_project")).isEmpty());
  ASSERT_EQ(ImportAndWait(raw_files).value("state").toString(), QStringLiteral("succeeded"));
  const QJsonArray items = ListItems();
  ASSERT_EQ(items.size(), static_cast<qsizetype>(raw_files.size()));
  for (const QJsonValue& value : items) {
    EXPECT_EQ(value.toObject().value("color_labels").toArray(), QJsonArray{});
    EXPECT_EQ(value.toObject().value("delete_mark"), QJsonValue(false));
  }
  const QJsonValue first  = items.at(0).toObject().value("element_id");
  const QJsonValue second = items.at(1).toObject().value("element_id");
  const QJsonValue third  = items.at(2).toObject().value("element_id");
  const QJsonArray three{first, second, third};
  const auto       info = [&]() {
    return MarksById(Result(QStringLiteral("library.info"), QJsonObject{{"element_ids", three}})
                               .value("items")
                               .toArray());
  };
  const QString succeeded = QStringLiteral("succeeded");

  // Blue on the first photo, then red on the first two: the first keeps blue.
  EXPECT_EQ(
      MarkAndWait(QStringLiteral("library.color_label"),
                  QJsonObject{{"element_ids", QJsonArray{first}}, {"code", "blue"}, {"on", true}}),
      succeeded);
  EXPECT_EQ(
      MarkAndWait(
          QStringLiteral("library.color_label"),
          QJsonObject{{"element_ids", QJsonArray{first, second}}, {"code", "red"}, {"on", true}}),
      succeeded);
  auto marks = info();
  EXPECT_EQ(marks.value(first.toDouble()).first, (QJsonArray{"red", "blue"}));
  EXPECT_EQ(marks.value(second.toDouble()).first, QJsonArray{"red"});
  EXPECT_EQ(marks.value(third.toDouble()).first, QJsonArray{});

  // Red off on both: blue stays on the first photo.
  EXPECT_EQ(
      MarkAndWait(
          QStringLiteral("library.color_label"),
          QJsonObject{{"element_ids", QJsonArray{first, second}}, {"code", "red"}, {"on", false}}),
      succeeded);
  marks = info();
  EXPECT_EQ(marks.value(first.toDouble()).first, QJsonArray{"blue"});
  EXPECT_EQ(marks.value(second.toDouble()).first, QJsonArray{});

  EXPECT_EQ(MarkAndWait(QStringLiteral("library.delete_mark"),
                        QJsonObject{{"element_ids", QJsonArray{first, second}}, {"on", true}}),
            succeeded);
  marks = info();
  EXPECT_TRUE(marks.value(first.toDouble()).second);
  EXPECT_TRUE(marks.value(second.toDouble()).second);
  EXPECT_FALSE(marks.value(third.toDouble()).second);

  const auto list = [&](QJsonObject params) {
    params.insert("limit", 1000);
    return Result(QStringLiteral("library.list"), params);
  };
  const QJsonObject deleted = list(QJsonObject{{"deleted", true}});
  EXPECT_EQ(deleted.value("total").toInt(), 2);
  EXPECT_EQ(ElementIds(deleted.value("items").toArray()),
            (QSet<double>{first.toDouble(), second.toDouble()}));
  const QJsonObject kept = list(QJsonObject{{"deleted", false}});
  EXPECT_EQ(kept.value("total").toInt(), items.size() - 2);
  EXPECT_FALSE(ElementIds(kept.value("items").toArray()).contains(first.toDouble()));
  const QJsonObject blue = list(QJsonObject{{"color_label", "blue"}});
  EXPECT_EQ(blue.value("total").toInt(), 1);
  EXPECT_EQ(ElementIds(blue.value("items").toArray()), QSet<double>{first.toDouble()});
  EXPECT_EQ(blue.value("items").toArray().at(0).toObject().value("color_labels").toArray(),
            QJsonArray{"blue"});
  EXPECT_EQ(list(QJsonObject{{"color_label", "blue"}, {"deleted", true}}).value("total").toInt(),
            1);
  EXPECT_EQ(list(QJsonObject{{"color_label", "blue"}, {"deleted", false}}).value("total").toInt(),
            0);
  EXPECT_EQ(list(QJsonObject{{"color_label", "red"}}).value("total").toInt(), 0);

  // Clearing the mark of one photo leaves the other marked.
  EXPECT_EQ(MarkAndWait(QStringLiteral("library.delete_mark"),
                        QJsonObject{{"element_ids", QJsonArray{first}}, {"on", false}}),
            succeeded);
  const QJsonObject still_deleted = list(QJsonObject{{"deleted", true}});
  EXPECT_EQ(still_deleted.value("total").toInt(), 1);
  EXPECT_EQ(ElementIds(still_deleted.value("items").toArray()), QSet<double>{second.toDouble()});
  // The mark changes nothing on disk: every photo is still in the project.
  EXPECT_EQ(list(QJsonObject{}).value("total").toInt(), items.size());
  Stop();
}

TEST_F(AutomationLibraryE2ETest, ColorLabelNamesRenameAndRestore) {
  ASSERT_FALSE(StartWithNewProject(QStringLiteral("names_project")).isEmpty());
  const QJsonArray  codes = Result(QStringLiteral("library.color_labels")).value("codes").toArray();
  const QStringList keys{"red", "orange", "yellow", "green", "blue", "purple", "gray"};
  ASSERT_EQ(codes.size(), keys.size());
  for (qsizetype index = 0; index < codes.size(); ++index) {
    const QJsonObject code = codes.at(index).toObject();
    EXPECT_EQ(code.value("code").toString(), keys.at(index));
    EXPECT_EQ(code.value("index").toInt(), index);
    EXPECT_FALSE(code.value("renamed").toBool());
    EXPECT_FALSE(code.value("default_name").toString().isEmpty());
    EXPECT_EQ(code.value("name"), code.value("default_name"));
    EXPECT_EQ(code.value("color").toString().size(), 7);
  }
  EXPECT_EQ(codes.at(0).toObject().value("color").toString(), QStringLiteral("#E5534B"));
  const QJsonValue  green_default = codes.at(3).toObject().value("default_name");

  const QJsonObject renamed       = Result(QStringLiteral("library.color_label_name.set"),
                                           QJsonObject{{"code", "green"}, {"name", "  Client picks  "}});
  EXPECT_EQ(renamed.value("code").toString(), QStringLiteral("green"));
  EXPECT_EQ(renamed.value("name").toString(), QStringLiteral("Client picks"));
  EXPECT_TRUE(renamed.value("renamed").toBool());
  EXPECT_EQ(renamed.value("default_name"), green_default);
  EXPECT_EQ(Result(QStringLiteral("library.color_labels")).value("codes").toArray().at(3),
            QJsonValue(renamed));

  const QJsonObject restored = Result(QStringLiteral("library.color_label_name.set"),
                                      QJsonObject{{"code", "green"}, {"name", ""}});
  EXPECT_EQ(restored.value("name"), green_default);
  EXPECT_FALSE(restored.value("renamed").toBool());
  EXPECT_EQ(Result(QStringLiteral("library.color_labels")).value("codes").toArray(), codes);

  const CliRun unknown =
      Call(QStringLiteral("library.color_label"),
           QJsonObject{{"element_ids", QJsonArray{1}}, {"code", "pink"}, {"on", true}});
  EXPECT_EQ(unknown.exit_code, 1);
  EXPECT_EQ(unknown.json.value("error").toObject().value("code").toInt(), -32602);
  Stop();
}

}  // namespace
}  // namespace alcedo::automation
