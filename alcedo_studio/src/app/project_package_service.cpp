//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/project_package_service.hpp"

#include "app/project_package_backend.hpp"

namespace alcedo {

auto ProjectPackageService::IsMetadataJsonPath(const std::filesystem::path& path) const -> bool {
  return project_pack::IsMetadataJsonPath(path);
}

auto ProjectPackageService::IsPackedProjectPath(const std::filesystem::path& path) const -> bool {
  return project_pack::IsPackedProjectPath(path);
}

auto ProjectPackageService::IsPackedProjectFile(const std::filesystem::path& path) const -> bool {
  return project_pack::IsPackedProjectFile(path);
}

auto ProjectPackageService::IsSupportedProjectFile(const std::filesystem::path& path) const -> bool {
  return project_pack::IsSupportedProjectFile(path);
}

auto ProjectPackageService::BuildUniquePackedProjectPath(const std::filesystem::path& folder,
                                                         const QString& project_name,
                                                         QString* error_out) const
    -> std::optional<std::filesystem::path> {
  return project_pack::BuildUniquePackedProjectPath(folder, project_name, error_out);
}

auto ProjectPackageService::BuildBundlePathFromMetaPath(const std::filesystem::path& meta_path) const
    -> std::filesystem::path {
  return project_pack::BuildBundlePathFromMetaPath(meta_path);
}

auto ProjectPackageService::CreateProjectWorkspace(const QString&                      project_name,
                                                   std::filesystem::path*              workspace_out,
                                                   project_pack::ProjectWorkspaceLock* lock_out,
                                                   QString* error_out) const -> bool {
  return project_pack::CreateProjectWorkspace(project_name, workspace_out, lock_out, error_out);
}

auto ProjectPackageService::BuildRuntimeProjectPair(const std::filesystem::path& workspace,
                                                    const QString& project_name) const
    -> std::pair<std::filesystem::path, std::filesystem::path> {
  return project_pack::BuildRuntimeProjectPair(workspace, project_name);
}

auto ProjectPackageService::BuildTempDbSnapshotPath(std::filesystem::path* snapshot_path_out,
                                                    QString* error_out) const -> bool {
  return project_pack::BuildTempDbSnapshotPath(snapshot_path_out, error_out);
}

auto ProjectPackageService::CreateLiveDbSnapshot(const std::shared_ptr<ProjectService>& project,
                                                 const std::filesystem::path& snapshot_path,
                                                 QString* error_out) const -> bool {
  return project_pack::CreateLiveDbSnapshot(project, snapshot_path, error_out);
}

auto ProjectPackageService::WritePackedProject(const std::filesystem::path& packed_path,
                                               const std::filesystem::path& meta_path,
                                               const std::filesystem::path& db_path,
                                               QString* error_out) const -> bool {
  return project_pack::WritePackedProject(packed_path, meta_path, db_path, error_out);
}

auto ProjectPackageService::UnpackProjectToWorkspace(const std::filesystem::path& packed_path,
                                                     const std::filesystem::path& workspace_dir,
                                                     const QString& project_name,
                                                     std::filesystem::path* db_path_out,
                                                     std::filesystem::path* meta_path_out,
                                                     QString* error_out) const -> bool {
  return project_pack::UnpackProjectToWorkspace(packed_path, workspace_dir, project_name,
                                                db_path_out, meta_path_out, error_out);
}

}  // namespace alcedo
