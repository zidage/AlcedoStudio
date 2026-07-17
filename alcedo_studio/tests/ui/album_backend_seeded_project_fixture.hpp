//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/// @file album_backend_seeded_project_fixture.hpp
/// @brief Shared GPU-free seeded-project helpers for UI tests that need a real
///        (synthetic DNG) image in storage — e.g. exercising the real delete
///        entry or folder-filter round-trips without RAW fixtures or GPU decode.
///
/// Extracted from album_backend_image_delete_test.cpp so workspace_shell_test.cpp
/// and the delete tests share one implementation. Header-only inline functions in
/// alcedo::ui::test; include from any UI test that links AlbumBackendLib.

#pragma once

#include <QSignalSpy>
#include <QString>
#include <QVariantList>

#include <filesystem>
#include <memory>
#include <optional>

#include "app/project_package_backend.hpp"
#include "app/project_service.hpp"
#include "image/image.hpp"
#include "sleeve/sleeve_element/sleeve_file.hpp"
#include "ui/album_backend_test_fixture.hpp"
#include "ui/alcedo_main/album_backend/import_export.hpp"
#include "ui/alcedo_main/album_backend/project_module.hpp"

namespace alcedo::ui::test {

/// A packed .alcd project containing one synthetic image, plus its ids.
struct SeededProject {
  std::filesystem::path packed_path_{};
  sl_element_id_t       file_id_  = 0;
  image_id_t            image_id_ = 0;
};

/// Pump the event loop until the import finishes (or timeout). Used by tests that
/// drive the real import path with RAW fixtures.
inline void WaitForImportFinished(ApplicationModuleHost& backend, int timeoutMs = 30000) {
  QSignalSpy spy(backend.import_export(), &ImportExportHandler::ImportStateChanged);
  const int  step    = 200;
  int        elapsed = 0;
  while (backend.import_export()->ImportRunning() && elapsed < timeoutMs) {
    spy.wait(step);
    elapsed += step;
  }
  ProcessEvents(600);
}

/// Locate the (single) .alcd file inside @p dir, if any.
inline auto FindPackedProjectPath(const std::filesystem::path& dir)
    -> std::optional<std::filesystem::path> {
  for (const auto& entry : std::filesystem::directory_iterator(dir)) {
    if (entry.is_regular_file() && entry.path().extension() == ".alcd") {
      return entry.path();
    }
  }
  return std::nullopt;
}

/// Look up a folder's UI id by name from a FolderController::Folders() result list.
inline auto FindFolderId(const QVariantList& folders, const QString& name) -> uint {
  for (const auto& v : folders) {
    const auto map = v.toMap();
    if (map.value("name").toString() == name) {
      return map.value("folderId").toUInt();
    }
  }
  return 0;
}

/// Build a packed project with one synthetic DNG image (no real pixels, no GPU).
/// Returns the packed path and the image's element/file + image ids on success.
inline auto CreateSeededPackedProject(const std::filesystem::path& tempDir)
    -> std::optional<SeededProject> {
  const auto db_path     = tempDir / "album_delete_seed.db";
  const auto meta_path   = tempDir / "album_delete_seed.json";
  const auto packed_path = tempDir / "album_delete_seed.alcd";

  auto project = std::make_shared<ProjectService>(db_path, meta_path, ProjectOpenMode::kCreateNew);
  auto image_handle = project->GetImagePoolService()->CreateAndReturnPinnedEmpty();
  if (!image_handle) {
    return std::nullopt;
  }

  auto image         = image_handle.Get();
  image->image_path_ = tempDir / "album-delete.dng";
  image->image_name_ = L"album-delete.dng";
  image->image_type_ = ImageType::DNG;

  ExifDisplayMetaData metadata;
  metadata.model_         = "Synthetic Album Camera";
  metadata.lens_          = "Synthetic 50mm";
  metadata.date_time_str_ = "2026-05-25 10:00:00";
  image->SetExifDisplayMetaData(std::move(metadata));

  auto file = project->GetSleeveService()->Write<std::shared_ptr<SleeveFile>>(
      [image](FileSystem& fs) -> std::shared_ptr<SleeveFile> {
        auto created       = fs.CreateFileInLibrary(image->image_name_);
        created->image_id_ = image->image_id_;
        return created;
      });
  if (!file.second.success_ || !file.first) {
    return std::nullopt;
  }

  const sl_element_id_t file_id  = file.first->element_id_;
  const image_id_t      image_id = image->image_id_;

  const auto image_sync = project->GetImagePoolService()->SyncWithStorage();
  if (!image_sync.failed_images_.empty()) {
    return std::nullopt;
  }
  project->SaveProject(meta_path);

  std::filesystem::path snapshot_path;
  if (!project_pack::BuildTempDbSnapshotPath(&snapshot_path, nullptr)) {
    return std::nullopt;
  }
  if (!project_pack::CreateLiveDbSnapshot(project, snapshot_path, nullptr)) {
    return std::nullopt;
  }
  const bool packed =
      project_pack::WritePackedProject(packed_path, meta_path, snapshot_path, nullptr);
  std::error_code ec;
  std::filesystem::remove(snapshot_path, ec);
  if (!packed) {
    return std::nullopt;
  }

  return SeededProject{packed_path, file_id, image_id};
}

/// Load a packed .alcd project into @p backend and wait for ServiceReady.
inline auto LoadPackedProject(ApplicationModuleHost& backend,
                              const std::filesystem::path& packedPath) -> bool {
  QSignalSpy project_spy(backend.project(), &ProjectModule::ProjectChanged);
  if (!backend.project()->LoadProject(PathToQString(packedPath))) {
    return false;
  }
  WaitForSignal(project_spy, 15000);
  ProcessEvents(500);
  return backend.project()->ServiceReady();
}

}  // namespace alcedo::ui::test