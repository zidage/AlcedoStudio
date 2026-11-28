// Copyright 2026 Yurun Zi
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "app/project_package_service.hpp"
#include "app/project_service.hpp"
#include "storage/store/sleeve/project_maintenance_store.hpp"

namespace alcedo {
enum class MaintenancePackageStage { Backup, Removal, ResultWrite, Publication, Restoration };
/// Independent package artifacts for one reviewed removal. Backup is immutable recovery data.
/// Workspace contains an independently editable database for this operation only. It holds no
/// live Image/history owners. Retain artifacts until activation or completed restoration.
class ProjectMaintenancePackage {
 public:
  using StageObserver    = std::function<void(MaintenancePackageStage)>;
  using ProgressObserver = std::function<void(size_t, size_t)>;
  /// Observers run on the package worker. Progress counts transaction work before commit;
  /// the caller must still wait for verified package activation before reporting removal.
  explicit ProjectMaintenancePackage(StageObserver observer = {}, ProgressObserver progress = {})
      : observer_(std::move(observer)), progress_(std::move(progress)) {}
  ~ProjectMaintenancePackage();
  /// Caller holds the project package write lock and has settled editor and other writers.
  /// Save without history cleanup, back up, revalidate, mutate only staging, verify, and publish.
  /// Throws with the real stage error. Installed() distinguishes errors that need restoration.
  void Apply(const std::shared_ptr<ProjectService>& project, const std::filesystem::path& package,
             std::span<const MaintenanceRemoval> removals,
             const std::filesystem::path&        backup_directory = {});
  /// Restore only when the original still has this operation's installed SHA-256. Caller
  /// holds package write exclusion. Throws and retains evidence if ownership is uncertain.
  void Restore();
  /// After activation, require the captured project identity and the exact installed (or
  /// restored) package digest before publishing a terminal result. Throws on external change.
  void VerifyActivatedProject(const ProjectService& project) const;
  /// Allow workspace removal after successful activation or successful baseline reopening.
  void Finish();
  bool Installed() const { return installed_; }
  bool RecoveryRequired() const { return recovery_required_; }
  bool BackupVerified() const { return backup_verified_; }
  auto BackupPath() const -> const std::filesystem::path& { return backup_; }
  auto RemovedIds() const -> const std::vector<sl_element_id_t>& { return removed_; }
  /// SHA-256 streamed in bounded chunks. Throws on any read failure.
  static auto Digest(const std::filesystem::path& path) -> std::string;
  /// Atomically replace an existing package, never remove it first. Same filesystem required.
  static void Replace(const std::filesystem::path& replacement, const std::filesystem::path& target,
                      const std::string& expected_replacement, const std::string& expected_target);

 private:
  void                               Stage(MaintenancePackageStage stage);
  ProjectPackageService              packages_;
  StageObserver                      observer_;
  ProgressObserver                   progress_;
  std::filesystem::path              original_, backup_, workspace_, result_;
  project_pack::ProjectWorkspaceLock workspace_lock_;
  std::string                        baseline_digest_, installed_digest_;
  std::vector<sl_element_id_t>       removed_;
  bool                               installed_         = false;
  bool                               recovery_required_ = false;
  bool                               backup_verified_   = false;
  bool                               cleanup_           = false;
};
}  // namespace alcedo
