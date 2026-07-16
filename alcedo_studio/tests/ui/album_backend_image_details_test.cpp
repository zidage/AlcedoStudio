//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/album_backend_test_fixture.hpp"

namespace alcedo::ui::test {
namespace {

using ImageDetailsTests = ApplicationModuleHostTestFixture;

void WaitForImportFinished(ApplicationModuleHost& backend, int timeoutMs = 30000) {
  QSignalSpy spy(backend.import_export(), &ImportExportHandler::ImportStateChanged);
  const int  step    = 200;
  int        elapsed = 0;
  while (backend.import_export()->ImportRunning() && elapsed < timeoutMs) {
    spy.wait(step);
    elapsed += step;
  }
  ProcessEvents(600);
}

auto FindRowValue(const QVariantList& rows, const QString& label) -> QString {
  for (const QVariant& rowVar : rows) {
    const QVariantMap row = rowVar.toMap();
    if (row.value("label").toString() == label) {
      return row.value("value").toString();
    }
  }
  return {};
}

auto FindRow(const QVariantList& rows, const QString& label) -> QVariantMap {
  for (const QVariant& rowVar : rows) {
    const QVariantMap row = rowVar.toMap();
    if (row.value("label").toString() == label) {
      return row;
    }
  }
  return {};
}

TEST_F(ImageDetailsTests, GetImageDetails_ReturnsStructuredExifSummary) {
  ApplicationModuleHost backend;
  ASSERT_TRUE(CreateTestProject(backend, "image_details"));

  auto images = CollectRawTestImages("airplane", 1);
  if (images.empty()) {
    GTEST_SKIP() << "No RAW images available in raw/airplane/";
  }

  backend.import_export()->StartImport(PathsToQStringList(images));
  WaitForImportFinished(backend);
  ASSERT_FALSE(backend.library()->Thumbnails().isEmpty());

  const QVariantMap first     = backend.library()->Thumbnails().front().toMap();
  const uint        elementId = first.value("elementId").toUInt();
  const uint        imageId   = first.value("imageId").toUInt();

  const QVariantMap result = backend.images()->GetImageDetails(elementId, imageId);
  ASSERT_TRUE(result.value("success").toBool());
  EXPECT_FALSE(result.value("title").toString().isEmpty());

  const QVariantList rows = result.value("rows").toList();
  ASSERT_GE(rows.size(), 10);
  EXPECT_FALSE(FindRowValue(rows, "Original Size").isEmpty());
  EXPECT_FALSE(FindRowValue(rows, "Original Aspect Ratio").isEmpty());
  EXPECT_FALSE(FindRowValue(rows, "Camera Model").isEmpty());
  EXPECT_FALSE(FindRowValue(rows, "Lens Model").isEmpty());
  EXPECT_FALSE(FindRowValue(rows, "Captured At").isEmpty());

  const QVariantMap sourceRow = FindRow(rows, "Source Directory");
  ASSERT_FALSE(sourceRow.isEmpty());
  EXPECT_EQ(sourceRow.value("value").toString(), PathToQString(images.front().parent_path()));
  EXPECT_EQ(sourceRow.value("actionId").toString(), QStringLiteral("open-directory"));
  EXPECT_EQ(sourceRow.value("actionValue").toString(), PathToQString(images.front().parent_path()));
}

TEST_F(ImageDetailsTests, GetImageDetails_RejectsInvalidIds) {
  ApplicationModuleHost backend;
  ASSERT_TRUE(CreateTestProject(backend, "image_details_invalid_ids"));

  const QVariantMap result = backend.images()->GetImageDetails(0, 0);
  EXPECT_FALSE(result.value("success").toBool());
  EXPECT_FALSE(result.value("message").toString().isEmpty());
}

TEST_F(ImageDetailsTests, GetImageDetails_FailsWithoutLoadedProject) {
  ApplicationModuleHost backend;

  const QVariantMap result = backend.images()->GetImageDetails(1, 1);
  EXPECT_FALSE(result.value("success").toBool());
  EXPECT_FALSE(result.value("message").toString().isEmpty());
}

}  // namespace
}  // namespace alcedo::ui::test
