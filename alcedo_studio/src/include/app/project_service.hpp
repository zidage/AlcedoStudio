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

 private:
  void                                  RecreateSleeveService(sl_element_id_t start_id);
  void                                  RegisterSemanticSearchProvider();

  std::filesystem::path                 db_path_;
  std::filesystem::path                 meta_path_;
  std::string                           project_uuid_;
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
