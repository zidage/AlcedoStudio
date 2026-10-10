//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Editor commands and render.preview through real alcedo-cli and alcedo_main --headless
// processes on a CI RAW file. The CI preset runs these tests (label ci_automation_flow) with the
// default editor backend of the platform; on Windows a build with CUDA uses CUDA.

#include <gtest/gtest.h>

#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QStringList>
#include <algorithm>
#include <cmath>

#include "automation_e2e_session.hpp"
#include "ui/editor_rhi/editor_backend.hpp"

namespace alcedo::automation {
namespace {

using test::CliRun;
using test::CollectCiRawFiles;

class AutomationEditorE2ETest : public test::AutomationE2ETestBase {
 protected:
  void SetUp() override {
    AutomationE2ETestBase::SetUp();
#ifdef _WIN32
    if (editor_rhi::IsBackendAvailableInThisBuild(editor_rhi::EditorBackend::Cuda)) {
      extra_host_options_ = QStringList{"--editor-backend", "cuda"};
    }
#endif
  }

  /// Starts a session with a new project that holds one CI RAW file and returns the project
  /// path. Sets element_ to the photo.
  auto StartWithOnePhoto(const QString& name) -> QString {
    const auto raw_files = CollectCiRawFiles();
    if (raw_files.empty()) {
      ADD_FAILURE() << "the CI RAW files are missing under " << TEST_IMG_PATH;
      return {};
    }
    const QString project_path = StartWithNewProject(name);
    if (project_path.isEmpty()) {
      return {};
    }
    EXPECT_EQ(ImportAndWait({raw_files.front()}).value("state").toString(),
              QStringLiteral("succeeded"));
    const QJsonArray items = ListItems();
    if (items.size() != 1) {
      ADD_FAILURE() << "expected one photo, listed " << items.size();
      return {};
    }
    element_ = items.first().toObject().value("element_id");
    return project_path;
  }

  /// Opens the photo in the editor and returns the editor.open result.
  auto OpenPhoto() -> QJsonObject {
    const QJsonObject opened =
        Result(QStringLiteral("editor.open"), QJsonObject{{"element_id", element_}});
    EXPECT_EQ(opened.value("state").toString(), QStringLiteral("Interactive"));
    return opened;
  }

  auto Value(const QString& field) -> double {
    return Result(QStringLiteral("editor.get"), QJsonObject{{"fields", QJsonArray{field}}})
        .value("values")
        .toObject()
        .value(field)
        .toDouble(std::nan(""));
  }

  auto CommitCount() -> qsizetype {
    return Result(QStringLiteral("editor.history")).value("commits").toArray().size();
  }

  auto Head() -> QJsonValue {
    return Result(QStringLiteral("editor.history")).value("head_commit");
  }

  auto Set(const QString& field, double value) -> QJsonObject {
    return Result(QStringLiteral("editor.set"), QJsonObject{{"field", field}, {"value", value}});
  }

  QJsonValue element_;
};

TEST_F(AutomationEditorE2ETest, SetExposureCreatesOneCommitAndReadsBackUiValue) {
  ASSERT_FALSE(StartWithOnePhoto(QStringLiteral("set_project")).isEmpty());
  OpenPhoto();
  const qsizetype  commits_before = CommitCount();
  const QJsonValue head_before    = Head();
  // A value equal to the current one changes nothing and makes no commit.
  ASSERT_GT(std::abs(Value(QStringLiteral("exposure")) - 0.5), 0.1);
  ASSERT_GT(std::abs(Value(QStringLiteral("saturation")) - 20), 1);

  const QJsonObject set = Set(QStringLiteral("exposure"), 0.5);
  EXPECT_NEAR(set.value("value").toDouble(), 0.5, 1e-6);
  EXPECT_NE(set.value("head_commit"), head_before);
  EXPECT_EQ(CommitCount(), commits_before + 1);
  EXPECT_EQ(Head(), set.value("head_commit"));
  EXPECT_NEAR(Value(QStringLiteral("exposure")), 0.5, 1e-6);

  // The issue example: a percent field in UI units.
  EXPECT_NEAR(Set(QStringLiteral("saturation"), 20).value("value").toDouble(), 20, 1e-4);
  EXPECT_NEAR(Value(QStringLiteral("saturation")), 20, 1e-4);
  EXPECT_EQ(CommitCount(), commits_before + 2);

  const QJsonValue  head = Head();
  const QJsonObject same = Set(QStringLiteral("saturation"), 20);
  EXPECT_EQ(same.value("head_commit"), head);
  EXPECT_EQ(CommitCount(), commits_before + 2);
  Stop();
}

TEST_F(AutomationEditorE2ETest, UndoRestoresPreviousUiValue) {
  ASSERT_FALSE(StartWithOnePhoto(QStringLiteral("undo_project")).isEmpty());
  OpenPhoto();
  const double before = Value(QStringLiteral("exposure"));
  ASSERT_GT(std::abs(before - 2.0), 0.1);
  const QJsonValue  head_before = Head();
  const QJsonValue  head_after  = Set(QStringLiteral("exposure"), 2.0).value("head_commit");

  const QJsonObject undone      = Result(QStringLiteral("editor.undo"));
  EXPECT_EQ(undone.value("head_commit"), head_before);
  EXPECT_TRUE(undone.value("can_redo").toBool());
  EXPECT_NEAR(Value(QStringLiteral("exposure")), before, 1e-6);

  const QJsonObject redone = Result(QStringLiteral("editor.redo"));
  EXPECT_EQ(redone.value("head_commit"), head_after);
  EXPECT_NEAR(Value(QStringLiteral("exposure")), 2.0, 1e-6);
  Stop();
}

TEST_F(AutomationEditorE2ETest, BatchStopsAtFirstInvalidValueAndReportsApplied) {
  ASSERT_FALSE(StartWithOnePhoto(QStringLiteral("batch_project")).isEmpty());
  OpenPhoto();
  const qsizetype commits_before  = CommitCount();
  const double    contrast_before = Value(QStringLiteral("contrast"));
  ASSERT_GT(std::abs(Value(QStringLiteral("exposure")) - 0.5), 0.1);
  ASSERT_GT(std::abs(contrast_before - 10), 1);

  const CliRun run =
      Call(QStringLiteral("editor.batch_set"),
           QJsonObject{{"changes", QJsonArray{QJsonObject{{"field", "exposure"}, {"value", 0.5}},
                                              QJsonObject{{"field", "saturation"}, {"value", 1000}},
                                              QJsonObject{{"field", "contrast"}, {"value", 10}}}}});
  EXPECT_EQ(run.exit_code, 1);
  const QJsonObject error = run.json.value("error").toObject();
  EXPECT_EQ(error.value("code").toInt(), -32602);
  const QJsonObject data = error.value("data").toObject();
  EXPECT_EQ(data.value("applied").toInt(), 1);
  EXPECT_EQ(data.value("pointer").toString(), QStringLiteral("/changes/1/value"));
  EXPECT_EQ(data.value("head_commit"), Head());
  EXPECT_EQ(CommitCount(), commits_before + 1);
  EXPECT_NEAR(Value(QStringLiteral("exposure")), 0.5, 1e-6);
  EXPECT_NEAR(Value(QStringLiteral("contrast")), contrast_before, 1e-6)
      << "changes after the failure stay out";

  const QJsonObject applied = Result(
      QStringLiteral("editor.batch_set"),
      QJsonObject{{"changes", QJsonArray{QJsonObject{{"field", "contrast"}, {"value", 10}},
                                         QJsonObject{{"field", "vibrance"}, {"value", -5}}}}});
  EXPECT_EQ(applied.value("applied").toInt(), 2);
  EXPECT_EQ(CommitCount(), commits_before + 3);
  EXPECT_NEAR(applied.value("values").toObject().value("vibrance").toDouble(), -5, 1e-4);
  Stop();
}

TEST_F(AutomationEditorE2ETest, SetBeforeInteractiveIsRejectedWithTheOwnerMessage) {
  ASSERT_FALSE(StartWithOnePhoto(QStringLiteral("rejected_project")).isEmpty());
  const CliRun run =
      Call(QStringLiteral("editor.set"), QJsonObject{{"field", "exposure"}, {"value", 1.0}});
  EXPECT_EQ(run.exit_code, 1);
  const QJsonObject error = run.json.value("error").toObject();
  EXPECT_EQ(error.value("code").toInt(), -32002);
  EXPECT_FALSE(error.value("message").toString().isEmpty());
  EXPECT_EQ(error.value("data").toObject().value("reason"), error.value("message"));
  Stop();
}

TEST_F(AutomationEditorE2ETest, PreviewWritesCurrentAndRootPngWithRequestedLongEdge) {
  ASSERT_FALSE(StartWithOnePhoto(QStringLiteral("preview_project")).isEmpty());
  OpenPhoto();
  ASSERT_GT(std::abs(Value(QStringLiteral("exposure")) - 1.0), 0.1);
  Set(QStringLiteral("exposure"), 1.0);
  const QString     out_dir = QDir(root_).filePath(QStringLiteral("preview"));
  const QJsonObject preview = Result(QStringLiteral("render.preview"),
                                     QJsonObject{{"out_dir", out_dir}, {"long_edge", 512}});
  for (const char* key : {"current", "root"}) {
    const QJsonObject file = preview.value(QLatin1String(key)).toObject();
    const QString     path = file.value("path").toString();
    EXPECT_TRUE(path.endsWith(QStringLiteral("_%1.png").arg(QLatin1String(key)))) << key;
    EXPECT_EQ(QFileInfo(path).absolutePath(), QFileInfo(out_dir).absoluteFilePath()) << key;
    const QImage image(path);
    ASSERT_FALSE(image.isNull()) << path.toStdString();
    EXPECT_EQ(std::max(image.width(), image.height()), 512) << key;
    EXPECT_EQ(file.value("width").toInt(), image.width()) << key;
    EXPECT_EQ(file.value("height").toInt(), image.height()) << key;
  }
  const QImage current(preview.value("current").toObject().value("path").toString());
  const QImage root(preview.value("root").toObject().value("path").toString());
  EXPECT_EQ(current.size(), root.size());
  EXPECT_NE(current, root) << "the current image has the exposure edit";
  Stop();
}

TEST_F(AutomationEditorE2ETest, RegionPreviewHasRegionAspectRatio) {
  ASSERT_FALSE(StartWithOnePhoto(QStringLiteral("region_project")).isEmpty());
  OpenPhoto();
  const QString     out_dir = QDir(root_).filePath(QStringLiteral("full"));
  const QJsonObject full    = Result(QStringLiteral("render.preview"),
                                     QJsonObject{{"out_dir", out_dir}, {"long_edge", 1024}})
                               .value("current")
                               .toObject();
  const double full_width  = full.value("width").toDouble();
  const double full_height = full.value("height").toDouble();
  ASSERT_GT(full_width, 0);
  ASSERT_GT(full_height, 0);

  const QJsonObject region{{"x", 0.1}, {"y", 0.2}, {"w", 0.5}, {"h", 0.25}};
  const QJsonObject part =
      Result(QStringLiteral("render.preview"),
             QJsonObject{{"out_dir", QDir(root_).filePath(QStringLiteral("region"))},
                         {"long_edge", 1024},
                         {"region", region}})
          .value("current")
          .toObject();
  const double width  = part.value("width").toDouble();
  const double height = part.value("height").toDouble();
  ASSERT_GT(width, 0);
  ASSERT_GT(height, 0);
  // The region is 0.5 by 0.25 of the image: height = width * (0.25 H) / (0.5 W), to a pixel.
  const double expected_height = width * (0.25 * full_height) / (0.5 * full_width);
  EXPECT_LE(std::abs(height - expected_height), 1.0)
      << width << "x" << height << " for a " << full_width << "x" << full_height << " image";

  const CliRun outside =
      Call(QStringLiteral("render.preview"),
           QJsonObject{{"out_dir", out_dir},
                       {"region", QJsonObject{{"x", 0.8}, {"y", 0.0}, {"w", 0.5}, {"h", 0.5}}}});
  EXPECT_EQ(outside.json.value("error").toObject().value("code").toInt(), -32602);
  Stop();
}

TEST_F(AutomationEditorE2ETest, EditsPersistAfterSessionRestart) {
  const QString project_path = StartWithOnePhoto(QStringLiteral("persist_project"));
  ASSERT_FALSE(project_path.isEmpty());
  OpenPhoto();
  ASSERT_GT(std::abs(Value(QStringLiteral("exposure")) - 0.75), 0.1);
  ASSERT_GT(std::abs(Value(QStringLiteral("saturation")) - 15), 1);
  Set(QStringLiteral("exposure"), 0.75);
  Set(QStringLiteral("saturation"), 15);
  const QJsonObject history = Result(QStringLiteral("editor.history"));
  EXPECT_TRUE(Result(QStringLiteral("editor.close")).value("closed").toBool());
  Stop();

  ASSERT_TRUE(StartWithProject(project_path));
  OpenPhoto();
  EXPECT_NEAR(Value(QStringLiteral("exposure")), 0.75, 1e-6);
  EXPECT_NEAR(Value(QStringLiteral("saturation")), 15, 1e-4);
  const QJsonObject reopened = Result(QStringLiteral("editor.history"));
  EXPECT_EQ(reopened.value("head_commit"), history.value("head_commit"));
  EXPECT_EQ(reopened.value("commits").toArray().size(), history.value("commits").toArray().size());
  Stop();
}

}  // namespace
}  // namespace alcedo::automation
