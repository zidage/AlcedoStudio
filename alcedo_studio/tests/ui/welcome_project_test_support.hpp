//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/// @file welcome_project_test_support.hpp
/// @brief Builders for the welcome project preview tests: packed projects with synthetic
/// files and edit history rows, an isolated recent-project settings store, and package file
/// state checks.

#pragma once

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QSettings>
#include <QSignalSpy>
#include <QString>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include <duckdb.h>

#include "app/project_package_backend.hpp"
#include "app/project_service.hpp"
#include "image/metadata.hpp"
#include "sleeve/sleeve_element/sleeve_file.hpp"
#include "sleeve/sleeve_filesystem.hpp"
#include "sleeve/storage.hpp"
#include "type/type.hpp"
#include "ui/album_backend_test_fixture.hpp"

namespace alcedo::ui::test {

/// Points the default QSettings store at an INI file in @p dir, so the recent-project list of
/// one test does not read or write the user's settings. Restores the native format on
/// destruction. Construct it before ApplicationModuleHost: ProjectModule reads the recent list
/// in its constructor.
class ScopedRecentProjectSettings {
 public:
  explicit ScopedRecentProjectSettings(const std::filesystem::path& dir) {
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       PathToQString(dir / "settings"));
    QCoreApplication::setOrganizationName(QStringLiteral("AlcedoTests"));
    QCoreApplication::setApplicationName(QStringLiteral("WelcomeProjectPreviewTest"));
    QSettings{}.remove(QStringLiteral("projects/recent"));
  }
  ~ScopedRecentProjectSettings() { QSettings::setDefaultFormat(QSettings::NativeFormat); }

  ScopedRecentProjectSettings(const ScopedRecentProjectSettings&)            = delete;
  ScopedRecentProjectSettings& operator=(const ScopedRecentProjectSettings&) = delete;
};

/// One synthetic photo file: an Image row with display metadata bound to a library file.
struct SyntheticPhotoSpec {
  file_name_t file_name_{};
  /// `YYYY-MM-DD HH:MM:SS`; empty writes a NULL capture date.
  std::string date_time_{};
};

/// Creates an Image through the image pool and a library file bound to it through the sleeve,
/// as import does. Returns the file element id.
inline auto AddSyntheticPhoto(ProjectService& project, const SyntheticPhotoSpec& spec)
    -> sl_element_id_t {
  auto image_pool          = project.GetImagePoolService();
  auto image               = image_pool->CreateAndReturnPinnedEmpty();
  auto image_id            = image.Get()->image_id_;
  image.Get()->image_name_ = spec.file_name_;
  image.Get()->image_path_ = std::filesystem::path{spec.file_name_};
  image.Get()->image_type_ = ImageType::DNG;
  ExifDisplayMetaData metadata;
  metadata.model_         = "Synthetic Camera";
  metadata.date_time_str_ = spec.date_time_;
  image.Get()->SetExifDisplayMetaData(std::move(metadata));
  image_pool->SyncWithStorage();

  auto file = project.GetSleeveService()->Write<std::shared_ptr<SleeveFile>>(
      [&spec, image_id](FileSystem& fs) -> std::shared_ptr<SleeveFile> {
        auto created       = fs.CreateFileInLibrary(spec.file_name_);
        created->image_id_ = image_id;
        return created;
      });
  EXPECT_TRUE(file.second.success_);
  EXPECT_NE(file.first, nullptr);
  return file.first ? file.first->element_id_ : 0;
}

/// Creates an Image row that no library file binds. The overview must not count it.
inline void AddUnboundImage(ProjectService& project, const file_name_t& file_name) {
  auto image_pool          = project.GetImagePoolService();
  auto image               = image_pool->CreateAndReturnPinnedEmpty();
  image.Get()->image_name_ = file_name;
  image.Get()->image_path_ = std::filesystem::path{file_name};
  image.Get()->image_type_ = ImageType::DNG;
  image_pool->SyncWithStorage();
}

/// Writes the edit history root of @p elementId (an `ImageEditState` row) and, when
/// @p commitCount is above zero, that many `EditCommit` rows on the root.
inline void AddEditHistoryRows(ProjectService& project, sl_element_id_t elementId,
                               int commitCount) {
  auto       guard   = project.GetStorage()->GetDatabase().GetConnectionGuard();
  const auto root_id = "root_" + std::to_string(elementId);
  const auto run     = [&guard](const std::string& sql) {
    duckdb_result result;
    const auto    state = duckdb_query(guard.conn_, sql.c_str(), &result);
    EXPECT_EQ(state, DuckDBSuccess) << duckdb_result_error(&result) << "\n" << sql;
    duckdb_destroy_result(&result);
  };
  run("INSERT INTO ImageEditState (element_id, root_id, active_version_id, "
      "materialized_head_commit_hash, materialized_transaction_chain_hash, "
      "serialized_pipeline_state, project_schema_version) VALUES (" +
      std::to_string(elementId) + ", '" + root_id + "', 'version_" + std::to_string(elementId) +
      "', NULL, 'chain', NULL, 1);");
  for (int i = 0; i < commitCount; ++i) {
    run("INSERT INTO EditCommit (commit_hash, root_id, first_parent_hash, second_parent_hash, "
        "created_at_ns, kind, edit_payload) VALUES ('commit_" +
        std::to_string(elementId) + "_" + std::to_string(i) + "', '" + root_id +
        "', NULL, NULL, 1, 0, '{}');");
  }
}

/// Creates a project in `<dir>/<name>_src`, lets @p populate add rows through the project
/// owners, saves it, and packs it to `<dir>/<name>.alcd`. Returns the package path.
inline auto BuildPackedProject(const std::filesystem::path& dir, const std::string& name,
                               const std::function<void(ProjectService&)>& populate = {})
    -> std::filesystem::path {
  const auto source_dir = dir / (name + "_src");
  std::filesystem::create_directories(source_dir);
  const auto db_path      = source_dir / (name + ".db");
  const auto meta_path    = source_dir / (name + ".json");
  const auto package_path = dir / (name + ".alcd");
  {
    auto project = std::make_shared<ProjectService>(db_path, meta_path,
                                                    ProjectOpenMode::kCreateNew);
    if (populate) {
      populate(*project);
    }
    project->GetSleeveService()->Sync();
    project->GetImagePoolService()->SyncWithStorage();
    project->SaveProject(meta_path);

    std::filesystem::path snapshot_path;
    EXPECT_TRUE(project_pack::BuildTempDbSnapshotPath(&snapshot_path, nullptr));
    EXPECT_TRUE(project_pack::CreateLiveDbSnapshot(project, snapshot_path, nullptr));
    EXPECT_TRUE(project_pack::WritePackedProject(package_path, meta_path, snapshot_path, nullptr));
    std::error_code ec;
    std::filesystem::remove(snapshot_path, ec);
  }
  return package_path;
}

/// Bytes and modification time of a package file. The time is moved one hour back first, so a
/// rewrite during the test always changes it.
struct PackageFileState {
  std::vector<char>               bytes_{};
  std::filesystem::file_time_type write_time_{};
};

inline auto ReadFileBytes(const std::filesystem::path& path) -> std::vector<char> {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

inline auto CapturePackageFileState(const std::filesystem::path& path) -> PackageFileState {
  std::filesystem::last_write_time(
      path, std::filesystem::last_write_time(path) - std::chrono::hours(1));
  return PackageFileState{ReadFileBytes(path), std::filesystem::last_write_time(path)};
}

/// Waits until no project load runs. Returns false on timeout.
inline bool WaitForProjectLoadIdle(ApplicationModuleHost& host, int timeoutMs = 15000) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  while (host.project()->ProjectLoading() && std::chrono::steady_clock::now() < deadline) {
    ProcessEvents(25);
  }
  // Deliver the queued notifications that follow the load completion.
  ProcessEvents(100);
  return !host.project()->ProjectLoading();
}

}  // namespace alcedo::ui::test
