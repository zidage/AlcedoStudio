//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/album_backend_test_fixture.hpp"

#include <QByteArray>
#include <QImage>
#include <QSignalSpy>

#include <algorithm>
#include <filesystem>
#include <functional>

#include "app/pipeline_service.hpp"
#include "app/project_service.hpp"
#include "edit/pipeline/default_pipeline_params.hpp"
#include "edit/pipeline/pipeline_accelerator.hpp"
#ifdef HAVE_METAL
#include "image/metal_image.hpp"
#endif

namespace alcedo::ui::test {
namespace {

using ThumbnailTests = ApplicationModuleHostTestFixture;

auto MetalAvailable() -> bool {
#ifdef HAVE_METAL
  auto* device = MTL::CreateSystemDefaultDevice();
  if (device == nullptr) {
    return false;
  }
  device->release();
  return true;
#else
  return false;
#endif
}

// The thumbnail suite is backend-agnostic: it exercises thumbnail lifecycle
// and re-request behavior, not accelerator throughput. Pin an explicit backend
// BEFORE the project pipeline captures the module preference so runs are
// deterministic and do not depend on first-use OpenCL kernel compilation
// latency (which can exceed the suite's wait windows). CUDA is used when the
// runtime supports it (the suite's historical default); otherwise the module's
// configured default stands.
void PinAcceleratorBeforeProjectOpen(ApplicationModuleHost& backend) {
  try {
    if (alcedo::ResolveAcceleratorBackend(AcceleratorBackendPreference::CUDA) ==
        GpuBackendKind::CUDA) {
      backend.project()->SetRuntimeAcceleratorPreference(AcceleratorBackendPreference::CUDA);
    }
  } catch (...) {
  }
}

void WaitForImportFinished(ApplicationModuleHost& backend, int timeoutMs = 30000) {
  QSignalSpy spy(backend.import_export(), &ImportExportHandler::ImportStateChanged);
  const int  step_ms = 200;
  int        elapsed = 0;
  while (backend.import_export()->ImportRunning() && elapsed < timeoutMs) {
    spy.wait(step_ms);
    elapsed += step_ms;
  }
  ProcessEvents(500);
}

auto WaitForThumbnailUrl(ApplicationModuleHost& backend, sl_element_id_t element_id,
                         bool expect_non_empty, int timeout_ms = 30000) -> QString {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    const QVariantList thumbnails = backend.library()->Thumbnails();
    for (const QVariant& row_value : thumbnails) {
      const QVariantMap row = row_value.toMap();
      if (static_cast<sl_element_id_t>(row.value("elementId").toUInt()) != element_id) {
        continue;
      }
      const QString thumb_url = row.value("thumbUrl").toString();
      if (expect_non_empty ? !thumb_url.isEmpty() : thumb_url.isEmpty()) {
        return thumb_url;
      }
      break;
    }
    ProcessEvents(100);
  }
  return {};
}

auto WaitForThumbnailRow(ApplicationModuleHost& backend, sl_element_id_t element_id,
                         const std::function<bool(const QVariantMap&)>& predicate,
                         int timeout_ms = 30000) -> QVariantMap {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    const QVariantList thumbnails = backend.library()->Thumbnails();
    for (const QVariant& row_value : thumbnails) {
      const QVariantMap row = row_value.toMap();
      if (static_cast<sl_element_id_t>(row.value("elementId").toUInt()) != element_id) {
        continue;
      }
      if (predicate(row)) {
        return row;
      }
      break;
    }
    ProcessEvents(100);
  }
  return {};
}

auto DecodeDataUrlImage(const QString& data_url) -> QImage {
  const int comma = data_url.indexOf(',');
  if (comma < 0) {
    return {};
  }

  const QByteArray encoded = data_url.mid(comma + 1).toLatin1();
  QImage           image;
  image.loadFromData(QByteArray::fromBase64(encoded));
  return image;
}

auto ResolveThumbUrlImage(ApplicationModuleHost& backend, const QString& thumb_url) -> QImage {
  if (thumb_url.startsWith(QStringLiteral("image://"))) {
    auto store = backend.library()->thumbs().image_store();
    if (!store) {
      return {};
    }
    return store->ResolveUrl(thumb_url);
  }
  return DecodeDataUrlImage(thumb_url);
}

auto MaxImageEdge(const QImage& image) -> int {
  return std::max(image.width(), image.height());
}

auto MakeAlbumItem(uint32_t index) -> AlbumItem {
  AlbumItem item;
  item.element_id = index + 1;
  item.file_id    = index + 1;
  item.image_id   = 1000 + index;
  item.folder_id  = 1;
  item.file_name  = QStringLiteral("image_%1.dng").arg(index + 1, 2, 10, QLatin1Char('0'));
  item.rating     = static_cast<int>(index % 5);
  return item;
}

auto MakeAlbumItems(uint32_t firstIndex, uint32_t count) -> std::vector<AlbumItem> {
  std::vector<AlbumItem> items;
  items.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    items.push_back(MakeAlbumItem(firstIndex + i));
  }
  return items;
}

}  // namespace

TEST_F(ThumbnailTests, ThumbnailModelSelectionRangeExtendsAfterPagedAppend) {
  AlbumThumbnailModel model;
  model.resetModel(MakeAlbumItems(0, 6), 12);

  EXPECT_EQ(model.count(), 6);
  EXPECT_EQ(model.totalCountInt(), 12);
  EXPECT_TRUE(model.hasMore());

  const QVariantList initially_loaded_range = model.getItemsInRange(2, 9);
  ASSERT_EQ(initially_loaded_range.size(), 4);
  EXPECT_EQ(initially_loaded_range.front().toMap().value("elementId").toUInt(), 3u);
  EXPECT_EQ(initially_loaded_range.back().toMap().value("elementId").toUInt(), 6u);

  model.appendPage(MakeAlbumItems(6, 6));

  EXPECT_EQ(model.count(), 12);
  EXPECT_FALSE(model.hasMore());

  const QVariantList completed_range = model.getItemsInRange(2, 9);
  ASSERT_EQ(completed_range.size(), 8);
  EXPECT_EQ(completed_range.front().toMap().value("elementId").toUInt(), 3u);
  EXPECT_EQ(completed_range.back().toMap().value("elementId").toUInt(), 10u);
  EXPECT_EQ(model.rowByElementId(10), 9);
}

TEST_F(ThumbnailTests, MetalThumbnailGridLifecycleWithGeometryOperatorsProducesProviderUrl) {
#ifndef HAVE_METAL
  GTEST_SKIP() << "Metal is not enabled in this build.";
#else
  if (!MetalAvailable()) {
    GTEST_SKIP() << "Metal device is unavailable in this environment.";
  }

  ApplicationModuleHost backend;
  ASSERT_TRUE(CreateTestProject(backend));

  auto images = CollectRawTestImages("still_life", 1);
  if (images.empty()) {
    images = CollectRawTestImages("airplane", 1);
  }
  if (images.empty()) {
    GTEST_SKIP() << "No RAW test image available for thumbnail regression.";
  }

  backend.import_export()->StartImport(PathsToQStringList(images));
  WaitForImportFinished(backend);

  ASSERT_FALSE(backend.import_export()->ImportRunning());
  ASSERT_GE(backend.library()->ShownCount(), 1);

  const QVariantMap first_row = backend.library()->Thumbnails().front().toMap();
  const auto        element_id =
      static_cast<sl_element_id_t>(first_row.value("elementId").toUInt());
  const auto image_id = static_cast<image_id_t>(first_row.value("imageId").toUInt());
  ASSERT_NE(element_id, 0);
  ASSERT_NE(image_id, 0);

  ProjectService project(db_path_, meta_path_);
  auto           pipeline_service = std::make_shared<PipelineMgmtService>(project.GetStorage());
  auto           pipeline_guard   = pipeline_service->LoadPipeline(element_id);
  ASSERT_NE(pipeline_guard, nullptr);
  ASSERT_NE(pipeline_guard->pipeline_, nullptr);

  auto exec = pipeline_guard->pipeline_;
  auto& global_params  = exec->GetGlobalParams();
  auto& loading_stage  = exec->GetStage(PipelineStageName::Image_Loading);
  auto& geometry_stage = exec->GetStage(PipelineStageName::Geometry_Adjustment);

  // The decode backend is a runtime property of the pipeline (resolved from
  // the accelerator preference); the params must not carry it.
  nlohmann::json raw_params = pipeline_defaults::MakeDefaultRawDecodeParams();
  raw_params["raw"]["backend"] = "alcedo";
  loading_stage.SetOperator(OperatorType::RAW_DECODE, raw_params);

  nlohmann::json crop_params = pipeline_defaults::MakeDefaultCropRotateParams();
  crop_params["crop_rotate"]["enabled"]     = true;
  crop_params["crop_rotate"]["enable_crop"] = true;
  crop_params["crop_rotate"]["angle_degrees"] = 0.0f;
  crop_params["crop_rotate"]["crop_rect"] = {
      {"x", 0.12f},
      {"y", 0.08f},
      {"w", 0.62f},
      {"h", 0.58f},
  };
  geometry_stage.SetOperator(OperatorType::CROP_ROTATE, crop_params, global_params);

  pipeline_guard->dirty_ = true;
  pipeline_service->SavePipeline(pipeline_guard);
  pipeline_service->Sync();

  QSignalSpy thumb_spy(backend.library(), &LibraryModule::ThumbnailUpdated);

  backend.library()->SetThumbnailVisible(static_cast<uint>(element_id), static_cast<uint>(image_id), true);

  ASSERT_TRUE(WaitForSignal(thumb_spy, 30000))
      << "Timed out waiting for ThumbnailUpdated after requesting visible thumbnail.";
  ProcessEvents(500);

  const QString first_thumb_url = WaitForThumbnailUrl(backend, element_id, true, 10000);
  ASSERT_FALSE(first_thumb_url.isEmpty())
      << "ThumbnailGridView-style pinning did not produce a thumbUrl.";

  const QVariantMap loaded_row = WaitForThumbnailRow(
      backend, element_id,
      [](const QVariantMap& row) {
        return !row.value("thumbUrl").toString().isEmpty() &&
               !row.value("thumbLoading").toBool() &&
               !row.value("thumbMissingSource").toBool();
      },
      10000);
  ASSERT_FALSE(loaded_row.isEmpty())
      << "Loaded thumbnail row did not clear loading state or unexpectedly marked source missing.";

  backend.library()->SetThumbnailVisible(static_cast<uint>(element_id), static_cast<uint>(image_id), false);
  ProcessEvents(500);

  const QString cleared_thumb_url = WaitForThumbnailUrl(backend, element_id, false, 5000);
  EXPECT_TRUE(cleared_thumb_url.isEmpty())
      << "ThumbnailGridView-style unpinning should clear the visible thumbUrl.";
#endif
}

TEST_F(ThumbnailTests, MissingSourceThumbnailStopsLoadingAndSetsMissingFlag) {
  ApplicationModuleHost backend;
  PinAcceleratorBeforeProjectOpen(backend);
  ASSERT_TRUE(CreateTestProject(backend));

  auto images = CollectRawTestImages("airplane", 1);
  if (images.empty()) {
    images = CollectRawTestImages("still_life", 1);
  }
  if (images.empty()) {
    GTEST_SKIP() << "No RAW test image available for missing-source thumbnail test.";
  }

  const auto copied_image = temp_dir_ / images.front().filename();
  std::filesystem::copy_file(images.front(), copied_image,
                             std::filesystem::copy_options::overwrite_existing);

  backend.import_export()->StartImport(PathsToQStringList({copied_image}));
  WaitForImportFinished(backend);

  ASSERT_FALSE(backend.import_export()->ImportRunning());
  ASSERT_GE(backend.library()->ShownCount(), 1);

  const QVariantMap first_row = backend.library()->Thumbnails().front().toMap();
  const auto        element_id =
      static_cast<sl_element_id_t>(first_row.value("elementId").toUInt());
  const auto image_id = static_cast<image_id_t>(first_row.value("imageId").toUInt());
  ASSERT_NE(element_id, 0);
  ASSERT_NE(image_id, 0);

  std::filesystem::remove(copied_image);

  QSignalSpy thumb_spy(backend.library(), &LibraryModule::ThumbnailUpdated);
  backend.library()->SetThumbnailVisible(static_cast<uint>(element_id), static_cast<uint>(image_id), true);

  ASSERT_TRUE(WaitForSignal(thumb_spy, 10000))
      << "Timed out waiting for ThumbnailUpdated after source file removal.";

  const QVariantMap missing_row = WaitForThumbnailRow(
      backend, element_id,
      [](const QVariantMap& row) {
        return row.value("thumbUrl").toString().isEmpty() &&
               !row.value("thumbLoading").toBool() &&
               row.value("thumbMissingSource").toBool();
      },
      10000);
  ASSERT_FALSE(missing_row.isEmpty())
      << "Missing-source thumbnail row did not settle into the expected error state.";
  EXPECT_FALSE(missing_row.value("thumbErrorText").toString().isEmpty())
      << "Missing-source thumbnail row should expose a user-visible error message.";

  backend.library()->SetThumbnailVisible(static_cast<uint>(element_id), static_cast<uint>(image_id), false);
  ProcessEvents(250);
}

TEST_F(ThumbnailTests, VisibleThumbnailRerequestsWhenMaxEdgeChanges) {
  ApplicationModuleHost backend;
  PinAcceleratorBeforeProjectOpen(backend);
  ASSERT_TRUE(CreateTestProject(backend));

  auto images = CollectRawTestImages("airplane", 1);
  if (images.empty()) {
    images = CollectRawTestImages("still_life", 1);
  }
  if (images.empty()) {
    GTEST_SKIP() << "No RAW test image available for zoom-tier thumbnail test.";
  }

  backend.import_export()->StartImport(PathsToQStringList(images));
  WaitForImportFinished(backend);

  ASSERT_FALSE(backend.import_export()->ImportRunning());
  ASSERT_GE(backend.library()->ShownCount(), 1);

  const QVariantMap first_row = backend.library()->Thumbnails().front().toMap();
  const auto        element_id =
      static_cast<sl_element_id_t>(first_row.value("elementId").toUInt());
  const auto image_id = static_cast<image_id_t>(first_row.value("imageId").toUInt());
  ASSERT_NE(element_id, 0);
  ASSERT_NE(image_id, 0);

  backend.library()->SetThumbnailVisible(static_cast<uint>(element_id), static_cast<uint>(image_id), true,
                              2048);
  const QString high_url = WaitForThumbnailUrl(backend, element_id, true, 30000);
  ASSERT_FALSE(high_url.isEmpty());
  EXPECT_TRUE(high_url.startsWith(QStringLiteral("image://alcedo-thumb/")))
      << high_url.toStdString();

  const QImage high_image = ResolveThumbUrlImage(backend, high_url);
  ASSERT_FALSE(high_image.isNull());
  ASSERT_LE(MaxImageEdge(high_image), 2048);

  backend.library()->SetThumbnailVisible(static_cast<uint>(element_id), static_cast<uint>(image_id), true,
                              256);
  const QVariantMap high_row = WaitForThumbnailRow(
      backend, element_id,
      [&high_url](const QVariantMap& row) {
        const QString url = row.value("thumbUrl").toString();
        return !url.isEmpty() && url != high_url && !row.value("thumbLoading").toBool();
      },
      30000);
  ASSERT_FALSE(high_row.isEmpty())
      << "Changing maxEdge on a pinned thumbnail did not refresh.";

  const QImage low_image = ResolveThumbUrlImage(backend, high_row.value("thumbUrl").toString());
  ASSERT_FALSE(low_image.isNull());
  EXPECT_LT(MaxImageEdge(low_image), MaxImageEdge(high_image));
  EXPECT_LE(MaxImageEdge(low_image), 256);

  backend.library()->SetThumbnailVisible(static_cast<uint>(element_id), static_cast<uint>(image_id), false,
                              256);
  ProcessEvents(250);
}

}  // namespace alcedo::ui::test
