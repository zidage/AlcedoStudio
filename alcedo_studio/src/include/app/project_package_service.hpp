//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "app/project_package_backend.hpp"

class QString;

namespace alcedo {
class ProjectService;

class ProjectPackageService {
 public:
  [[nodiscard]] auto IsMetadataJsonPath(const std::filesystem::path& path) const -> bool;
  [[nodiscard]] auto IsPackedProjectPath(const std::filesystem::path& path) const -> bool;
  [[nodiscard]] auto IsPackedProjectFile(const std::filesystem::path& path) const -> bool;
  [[nodiscard]] auto IsSupportedProjectFile(const std::filesystem::path& path) const -> bool;

  [[nodiscard]] auto BuildUniquePackedProjectPath(const std::filesystem::path& folder,
                                                  const QString& project_name,
                                                  QString* error_out) const
      -> std::optional<std::filesystem::path>;
  [[nodiscard]] auto BuildBundlePathFromMetaPath(const std::filesystem::path& meta_path) const
      -> std::filesystem::path;

  [[nodiscard]] auto CreateProjectWorkspace(const QString&                      project_name,
                                            std::filesystem::path*              workspace_out,
                                            project_pack::ProjectWorkspaceLock* lock_out,
                                            QString* error_out) const -> bool;
  [[nodiscard]] auto BuildRuntimeProjectPair(const std::filesystem::path& workspace,
                                             const QString& project_name) const
      -> std::pair<std::filesystem::path, std::filesystem::path>;

  [[nodiscard]] auto BuildTempDbSnapshotPath(std::filesystem::path* snapshot_path_out,
                                             QString* error_out) const -> bool;
  [[nodiscard]] auto CreateLiveDbSnapshot(const std::shared_ptr<ProjectService>& project,
                                          const std::filesystem::path& snapshot_path,
                                          QString* error_out) const -> bool;

  [[nodiscard]] auto WritePackedProject(const std::filesystem::path& packed_path,
                                        const std::filesystem::path& meta_path,
                                        const std::filesystem::path& db_path,
                                        QString* error_out) const -> bool;
  [[nodiscard]] auto UnpackProjectToWorkspace(const std::filesystem::path& packed_path,
                                              const std::filesystem::path& workspace_dir,
                                              const QString& project_name,
                                              std::filesystem::path* db_path_out,
                                              std::filesystem::path* meta_path_out,
                                              QString* error_out) const -> bool;
};

}  // namespace alcedo
