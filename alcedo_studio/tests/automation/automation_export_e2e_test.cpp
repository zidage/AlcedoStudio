//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// export.* commands through real alcedo-cli and alcedo_main --headless processes on a CI RAW
// file: the written files have the requested format, bit depth, and long edge, and options
// that break a rule are rejected with the allowed values before an export starts.

#include <OpenImageIO/imageio.h>
#include <gtest/gtest.h>

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>
#include <algorithm>
#include <memory>

#include "automation_editor_e2e_session.hpp"

namespace alcedo::automation {
namespace {

class AutomationExportE2ETest : public test::AutomationEditorE2ETestBase {
 protected:
  /// Runs export.run with @p options for the photo, waits for its task, and returns the
  /// finished task.
  auto ExportAndWait(const QString& out_dir, QJsonObject options) -> QJsonObject {
    options.insert("element_ids", QJsonArray{element_});
    options.insert("out_dir", out_dir);
    const QJsonObject started = Result(QStringLiteral("export.run"), options);
    const QString     task_id = started.value("task_id").toString();
    EXPECT_TRUE(task_id.startsWith(QStringLiteral("export-"))) << task_id.toStdString();
    EXPECT_EQ(started.value("queued").toInt(), 1);
    return Result(QStringLiteral("tasks.wait"),
                  QJsonObject{{"task_id", task_id}, {"timeout_ms", 600000}});
  }

  static auto FilesIn(const QString& folder) -> QStringList {
    return QDir(folder).entryList(QDir::Files, QDir::Name);
  }
};

TEST_F(AutomationExportE2ETest, ExportJpegWritesFilesWithRequestedLongEdge) {
  ASSERT_FALSE(StartWithOnePhoto(QStringLiteral("export_jpeg_project")).isEmpty());
  const QString     out_dir = QDir(root_).filePath(QStringLiteral("jpeg_out"));
  const QJsonObject finished =
      ExportAndWait(out_dir, QJsonObject{{"format", "JPEG"}, {"long_edge", 512}, {"quality", 90}});
  EXPECT_EQ(finished.value("kind").toString(), QStringLiteral("export"));
  EXPECT_EQ(finished.value("state").toString(), QStringLiteral("succeeded"))
      << finished.value("message").toString().toStdString();
  EXPECT_EQ(finished.value("counts").toObject().value("succeeded").toInt(), 1);
  EXPECT_EQ(finished.value("counts").toObject().value("failed").toInt(), 0);

  const QStringList files = FilesIn(out_dir);
  ASSERT_EQ(files.size(), 1) << files.join(", ").toStdString();
  EXPECT_TRUE(files.first().endsWith(QStringLiteral(".jpg")));
  auto input = OIIO::ImageInput::open(QDir(out_dir).filePath(files.first()).toStdString());
  ASSERT_TRUE(input) << OIIO::geterror();
  EXPECT_EQ(std::max(input->spec().width, input->spec().height), 512);
  input->close();

  const QJsonObject status = Result(QStringLiteral("export.status"));
  EXPECT_FALSE(status.value("in_flight").toBool());
  EXPECT_EQ(status.value("task_id"), finished.value("task_id"));
  const QJsonArray items = status.value("items").toArray();
  ASSERT_EQ(items.size(), 1);
  EXPECT_EQ(items.first().toObject().value("element_id"), element_);
  EXPECT_EQ(items.first().toObject().value("state").toString(), QStringLiteral("succeeded"));
  Stop();
}

TEST_F(AutomationExportE2ETest, Export16BitTiffWritesSixteenBitFile) {
  ASSERT_FALSE(StartWithOnePhoto(QStringLiteral("export_tiff_project")).isEmpty());
  const QString     out_dir = QDir(root_).filePath(QStringLiteral("tiff_out"));
  const QJsonObject finished =
      ExportAndWait(out_dir, QJsonObject{{"format", "TIFF"},
                                         {"bit_depth", 16},
                                         {"tiff_compression", "ZIP"},
                                         {"long_edge", 640},
                                         {"file_name_pattern", "tiff-{sequence:000}"}});
  EXPECT_EQ(finished.value("state").toString(), QStringLiteral("succeeded"))
      << finished.value("message").toString().toStdString();

  const QStringList files = FilesIn(out_dir);
  ASSERT_EQ(files, QStringList{QStringLiteral("tiff-001.tif")});
  const std::string path  = QDir(out_dir).filePath(files.first()).toStdString();
  auto              input = OIIO::ImageInput::open(path);
  ASSERT_TRUE(input) << OIIO::geterror();
  const OIIO::ImageSpec& spec = input->spec();
  EXPECT_EQ(spec.format, OIIO::TypeDesc::UINT16);
  EXPECT_EQ(std::max(spec.width, spec.height), 640);
  input->close();
  Stop();
}

TEST_F(AutomationExportE2ETest, InvalidOptionsAreRejectedWithAllowedValues) {
  ASSERT_FALSE(StartWithOnePhoto(QStringLiteral("export_reject_project")).isEmpty());
  const QString     out_dir = QDir(root_).filePath(QStringLiteral("rejected_out"));

  const QJsonObject options = Result(QStringLiteral("export.options"));
  EXPECT_EQ(options.value("defaults").toObject().value("format").toString(),
            QStringLiteral("JPEG"));
  EXPECT_EQ(options.value("bit_depths").toObject().value("EXR").toArray(), (QJsonArray{16, 32}));
  EXPECT_EQ(options.value("limits").toObject().value("long_edge").toObject().value("min").toInt(),
            256);

  const auto rejected = [&](QJsonObject params) {
    params.insert("element_ids", QJsonArray{element_});
    params.insert("out_dir", out_dir);
    const test::CliRun run = Call(QStringLiteral("export.run"), params);
    EXPECT_EQ(run.exit_code, 1);
    return run.json.value("error").toObject();
  };
  const QJsonObject long_edge = rejected(QJsonObject{{"long_edge", 100}});
  EXPECT_EQ(long_edge.value("code").toInt(), -32602);
  EXPECT_EQ(long_edge.value("message").toString(),
            QStringLiteral("long_edge must be in [256, 16384]"));
  EXPECT_EQ(long_edge.value("data").toObject().value("pointer").toString(),
            QStringLiteral("/long_edge"));

  const QJsonObject bit_depth = rejected(QJsonObject{{"format", "JPEG"}, {"bit_depth", 16}});
  EXPECT_EQ(bit_depth.value("code").toInt(), -32602);
  EXPECT_EQ(bit_depth.value("message").toString(),
            QStringLiteral("bit_depth for JPEG must be one of: 8"));

  const QJsonObject format = rejected(QJsonObject{{"format", "WEBP"}});
  EXPECT_EQ(format.value("code").toInt(), -32602);
  EXPECT_TRUE(format.value("message").toString().contains(QStringLiteral("JPEG, PNG, TIFF, EXR")))
      << format.value("message").toString().toStdString();

  const QJsonObject pattern = rejected(QJsonObject{{"file_name_pattern", "{unknown}"}});
  EXPECT_EQ(pattern.value("data").toObject().value("pointer").toString(),
            QStringLiteral("/file_name_pattern"));

  // No export started.
  EXPECT_FALSE(QFileInfo::exists(out_dir));
  EXPECT_TRUE(Result(QStringLiteral("tasks.list")).value("tasks").toArray().isEmpty());
  EXPECT_TRUE(Result(QStringLiteral("export.status")).value("task_id").toString().isEmpty());
  Stop();
}

}  // namespace
}  // namespace alcedo::automation
