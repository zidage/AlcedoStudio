//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>

#include "app/album_browse_service.hpp"
#include "app/ai_sidecar_runtime_service.hpp"
#include "app/project_mask_cache_service.hpp"
#include "app/sleeve_service.hpp"
#include "app/sleeve_filter_service.hpp"
#include "image_pool_service.hpp"
#include "sleeve/storage.hpp"
#include "type/type.hpp"

namespace alcedo {
class ProjectPackageService;
enum class ProjectOpenMode {
  kLoadOrCreate = 0,
  kLoadExisting,
  kCreateNew,
};

class ProjectService {
 public:
  ProjectService(const std::filesystem::path& db_path, const std::filesystem::path& meta_path,
                 ProjectOpenMode open_mode = ProjectOpenMode::kLoadOrCreate);
  ~ProjectService();

  void SaveProject(const std::filesystem::path& meta_path);
  void LoadProject(const std::filesystem::path& meta_path);

  auto GetStorage() const -> std::shared_ptr<Storage> { return storage_; }

  auto GetSleeveService() const -> std::shared_ptr<SleeveServiceImpl> { return sleeve_service_; }
  auto GetImagePoolService() const -> std::shared_ptr<ImagePoolService> {
    return pool_service_;
  }
  auto GetSleeveFilterService() const -> std::shared_ptr<SleeveFilterService> {
    return filter_service_;
  }
  auto GetAlbumBrowseService() const -> std::shared_ptr<AlbumBrowseService> {
    return browse_service_;
  }
  auto GetProjectPackageService() const -> std::shared_ptr<ProjectPackageService> {
    return package_service_;
  }
  auto GetAiSidecarRuntimeService() const -> std::shared_ptr<AiSidecarRuntimeService>;

  auto GetDBPath() const -> const std::filesystem::path& { return db_path_; }
  auto GetMetaPath() const -> const std::filesystem::path& { return meta_path_; }
  auto GetProjectUUID() const -> const std::string& { return project_uuid_; }

  /**
   * @brief Current Mix-cache root, retention, and leftover-cleanup state.
   *
   * Empty @ref ProjectMaskCacheSettings::chosen_root is resolved against this
   * project's metadata directory. Not part of photo history.
   */
  [[nodiscard]] auto GetMaskCacheSettings() const -> ProjectMaskCacheSettings;

  [[nodiscard]] auto GetMaskCacheService() -> ProjectMaskCacheService*;
  [[nodiscard]] auto GetMaskCacheService() const -> const ProjectMaskCacheService*;

  /**
   * @brief Validate @p chosen_root, persist it, then publish it to the cache writer.
   *
   * @param chosen_root User-selected root, or empty for the metadata directory.
   * @param expected_revision Must match the current settings revision.
   * @param error Receives the real filesystem or revision failure. Unchanged on success.
   * @return false when the previous root and files are retained.
   */
  auto SetMaskCacheRoot(std::filesystem::path chosen_root, std::uint64_t expected_revision,
                        std::string* error) -> bool;

  /**
   * @brief Persist Keep vs DeleteOnProjectClose. Does not add a photo Version.
   */
  auto SetMaskCacheRetention(ProjectMaskCacheRetention retention, std::uint64_t expected_revision,
                             std::string* error) -> bool;

  /**
   * @brief Delete this project's Mix-cache files. Stroke history and RAW stay on disk.
   */
  auto ClearMaskCache(std::string* error) -> bool;

  /**
   * @brief Flush coalesced Mix-cache writes at a save boundary.
   *
   * Parameter save must already have succeeded or be independent of this call.
   * A cache I/O failure does not rewrite project metadata or history.
   */
  auto FlushMaskCacheWrites(std::string* error) -> bool;

  /**
   * @brief After a successful parameter/WAL save, apply DeleteOnProjectClose if set.
   *
   * Waits for Mix-cache readers and writers. Cleanup failure does not mark the
   * parameter save as failed.
   */
  auto CloseAfterSuccessfulSave(std::string* error) -> bool;

  /**
   * @brief Explicit project-removal cache choice. Removing a recent-list row is
   *        not authorization to delete files; pass KeepFiles for that path.
   */
  auto ApplyMaskCacheRemovalChoice(ProjectMaskCacheRemovalChoice choice, std::string* error)
      -> bool;

 private:
  void                                  RecreateSleeveService(sl_element_id_t start_id);
  void                                  RegisterSemanticSearchProvider();
  void                                  RecreateMaskCacheService();
  auto                                  PersistCurrentMetadata(std::string* error) -> bool;
  void                                  FinishPendingMaskCacheCleanup();

  std::filesystem::path                 db_path_;
  std::filesystem::path                 meta_path_;
  std::string                           project_uuid_;
  ProjectMaskCacheSettings              mask_cache_settings_;
  std::unique_ptr<ProjectMaskCacheService> mask_cache_service_;
  std::shared_ptr<Storage>       storage_;
  std::shared_ptr<SleeveServiceImpl>    sleeve_service_;
  // TODO: Add ImagePoolService and store its start_id into the metadata
  std::shared_ptr<ImagePoolService>      pool_service_;
  std::shared_ptr<SleeveFilterService>   filter_service_;
  std::shared_ptr<AlbumBrowseService>    browse_service_;
  std::shared_ptr<ProjectPackageService> package_service_;
  mutable std::mutex                     ai_sidecar_runtime_mutex_;
  mutable std::shared_ptr<AiSidecarRuntimeService> ai_sidecar_runtime_service_;
};
};  // namespace alcedo
