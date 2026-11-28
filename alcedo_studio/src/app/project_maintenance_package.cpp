// Copyright 2026 Yurun Zi
// SPDX-License-Identifier: GPL-3.0-only
#include "app/project_maintenance_package.hpp"

#include <QCryptographicHash>
#include <QDate>
#include <QFile>
#include <fstream>
#include <map>
#include <stdexcept>
#include <system_error>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

#include "app/project_maintenance_service.hpp"

namespace alcedo {
namespace {
/// Propagate checked package errors with their original detail.
void Require(bool result, const QString& error) {
  if (!result)
    throw std::runtime_error(error.isEmpty() ? "Project package operation failed"
                                             : error.toStdString());
}
/// Read package metadata without creating a second live ProjectService.
auto Metadata(const std::filesystem::path& path) -> nlohmann::json {
  std::ifstream input(path);
  if (!input) throw std::runtime_error("Cannot read package metadata");
  return nlohmann::json::parse(input);
}
}  // namespace

void ProjectMaintenancePackage::Stage(MaintenancePackageStage stage) {
  if (observer_) observer_(stage);
}
auto ProjectMaintenancePackage::Digest(const std::filesystem::path& path) -> std::string {
  QFile file(QString::fromStdWString(path.wstring()));
  if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error(file.errorString().toStdString());
  QCryptographicHash hash(QCryptographicHash::Sha256);
  if (!hash.addData(&file)) throw std::runtime_error("Package digest read failed");
  return hash.result().toHex().toStdString();
}
void ProjectMaintenancePackage::Replace(const std::filesystem::path&        replacement,
                                        const std::filesystem::path&        target,
                                        [[maybe_unused]] const std::string& expected_replacement,
                                        [[maybe_unused]] const std::string& expected_target) {
#ifdef _WIN32
  constexpr int kRetryLimit = 3;
  for (int retries = 0;; ++retries) {
    if (ReplaceFileW(target.c_str(), replacement.c_str(), nullptr, 0, nullptr, nullptr)) return;
    const auto error = GetLastError();
    // This error preserves both names. Check both files before retrying the same operation.
    if (error != ERROR_UNABLE_TO_REMOVE_REPLACED || retries == kRetryLimit) {
      throw std::system_error(static_cast<int>(error), std::system_category(),
                              "Atomic package replacement");
    }
    Sleep(100);
    if (Digest(target) != expected_target || Digest(replacement) != expected_replacement) {
      throw std::runtime_error("Package files changed during replacement");
    }
  }
#else
  std::filesystem::rename(replacement, target);
#endif
}
ProjectMaintenancePackage::~ProjectMaintenancePackage() {
  workspace_lock_.Release();
  if (cleanup_ && !workspace_.empty()) {
    // Only the runtime workspace created by the package owner can be removed here.
    const auto root   = std::filesystem::weakly_canonical(project_pack::ProjectTempRoot());
    const auto target = std::filesystem::weakly_canonical(workspace_);
    if (target.parent_path() == root && target.filename().wstring().starts_with(L"runtime_")) {
      std::error_code error;
      std::filesystem::remove_all(target, error);
    }
  }
}
void ProjectMaintenancePackage::Apply(const std::shared_ptr<ProjectService>& project,
                                      const std::filesystem::path&           package,
                                      std::span<const MaintenanceRemoval>    removals,
                                      const std::filesystem::path&           backup_directory) {
  if (!project || removals.empty() || !original_.empty())
    throw std::invalid_argument("Invalid maintenance package request");
  original_ = package;
  QString    error;
  const auto sync = project->GetImagePoolService()->SyncWithStorage();
  if (!sync.failed_images_.empty())
    throw std::runtime_error("Image pool persistence failed before maintenance: " +
                             sync.failed_images_.front().message_);
  const auto library_sync = project->GetSleeveService()->Sync();
  if (!library_sync.success_)
    throw std::runtime_error("Library persistence failed before maintenance: " +
                             library_sync.message_);
  project->SaveProject(project->GetMetaPath());
  Require(packages_.CreateProjectWorkspace(QStringLiteral("maintenance"), &workspace_,
                                           &workspace_lock_, &error),
          error);
  const auto baseline_db = workspace_ / "baseline.db";
  Require(packages_.CreateLiveDbSnapshot(project, baseline_db, &error), error);
  // Baseline save uses the same package exclusion as every normal package writer.
  Require(packages_.WritePackedProject(original_, project->GetMetaPath(), baseline_db, &error),
          error);
  baseline_digest_ = Digest(original_);
  Stage(MaintenancePackageStage::Backup);
  const auto folder = backup_directory.empty() ? original_.parent_path() : backup_directory;
  const auto stem   = original_.stem().wstring() + L".backup-before-maintenance-" +
                      QDate::currentDate().toString(QStringLiteral("yyyy-MM-dd")).toStdWString() +
                      L"-";
  for (unsigned index = 1;; ++index) {
    backup_ = folder / (stem + std::to_wstring(index) + original_.extension().wstring());
    std::error_code ec;
    if (std::filesystem::copy_file(original_, backup_, std::filesystem::copy_options::none, ec))
      break;
    if (ec == std::errc::file_exists) continue;
    throw std::system_error(ec, "Create maintenance backup");
  }
  if (Digest(backup_) != baseline_digest_)
    throw std::runtime_error("Maintenance backup digest differs from baseline");
  std::filesystem::path staged_db, staged_meta;
  Require(packages_.UnpackProjectToWorkspace(backup_, workspace_, QStringLiteral("staged"),
                                             &staged_db, &staged_meta, &error),
          error);
  if (Metadata(staged_meta).at("project_uuid") != project->GetProjectUUID()) {
    throw std::runtime_error("Backup project identity differs from the loaded project");
  }
  backup_verified_ = true;
  std::map<std::string, std::string> expected;
  std::vector<image_id_t>            removed_images;
  {
    Storage                   storage(staged_db);
    ProjectMaintenanceService validation;
    validation.Scan(storage.GetProjectMaintenanceStore(),
                    project->GetDBPath().parent_path() / "editor-journal");
    std::vector<sl_element_id_t> ids;
    for (const auto& removal : removals) ids.push_back(removal.entry_id);
    const auto checked = validation.Selection(ids);
    for (size_t i = 0; i < checked.size(); ++i) {
      if (checked[i].retained_id != removals[i].retained_id) {
        throw std::runtime_error("Retained entry changed since review; scan again");
      }
    }
    auto& store    = storage.GetProjectMaintenanceStore();
    removed_images = store.ImageIds(removals);
    expected       = store.RetainedRowDigests(removals, removed_images, true);
    Stage(MaintenancePackageStage::Removal);
    removed_ = store.RemovePristineProjectEntries(removals, progress_);
    if (store.RetainedRowDigests(removals, removed_images, false) != expected) {
      throw std::runtime_error("Retained project rows changed during maintenance");
    }
    ProjectService::RefreshPackageDataSummary(staged_meta, storage);
    storage.Maintenance().Checkpoint();
  }
  Stage(MaintenancePackageStage::ResultWrite);
  result_ = workspace_ / "result.alcd";
  Require(packages_.WritePackedProject(result_, staged_meta, staged_db, &error), error);
  const auto verify_dir = workspace_ / "verify";
  std::filesystem::create_directory(verify_dir);
  std::filesystem::path verify_db, verify_meta;
  Require(packages_.UnpackProjectToWorkspace(result_, verify_dir, QStringLiteral("verified"),
                                             &verify_db, &verify_meta, &error),
          error);
  if (Metadata(verify_meta).at("project_uuid") != project->GetProjectUUID())
    throw std::runtime_error("Result project identity changed");
  {
    Storage                 verification(verify_db);
    auto&                   store = verification.GetProjectMaintenanceStore();
    if (store.RetainedRowDigests(removals, removed_images, false) != expected) {
      throw std::runtime_error("Reopened package retained-row verification failed");
    }
    for (auto id : removed_) {
      const auto evidence = store.Inspect(id);
      if (evidence.protection_reasons.empty() || !evidence.source_path.empty()) {
        throw std::runtime_error("Removed entry remains in verified package");
      }
    }
  }
  installed_digest_ = Digest(result_);
  const auto publication =
      original_.parent_path() /
      (original_.filename().wstring() + L".maintenance-" + workspace_.filename().wstring());
  std::filesystem::copy_file(result_, publication, std::filesystem::copy_options::none);
  if (Digest(publication) != installed_digest_)
    throw std::runtime_error("Publication copy digest differs from verified result");
  Stage(MaintenancePackageStage::Publication);
  if (Digest(original_) != baseline_digest_)
    throw std::runtime_error(
        "Saved baseline changed before publication; current package was left untouched");
  try {
    Replace(publication, original_, installed_digest_, baseline_digest_);
  } catch (...) {
    // A failed OS replacement can have an uncertain outcome. Observe actual bytes before
    // deciding whether restoration is permitted; never assume that failure means no write.
    try {
      const auto current = Digest(original_);
      installed_         = current == installed_digest_;
      recovery_required_ = !installed_ && current != baseline_digest_;
    } catch (...) {
      recovery_required_ = true;
    }
    throw;
  }
  installed_ = true;
  if (Digest(original_) != installed_digest_) {
    recovery_required_ = true;
    throw std::runtime_error("Installed package digest changed; recovery required");
  }
}
void ProjectMaintenancePackage::Restore() {
  if (!installed_)
    throw std::runtime_error("Maintenance did not install a package; restoration is not permitted");
  Stage(MaintenancePackageStage::Restoration);
  if (Digest(original_) != installed_digest_)
    throw std::runtime_error(
        "Published package changed externally; recovery required, current package was left "
        "untouched");
  if (Digest(backup_) != baseline_digest_)
    throw std::runtime_error("Backup digest changed; recovery required");
  const auto restore =
      original_.parent_path() / (original_.filename().wstring() + L".maintenance-restore");
  if (!std::filesystem::copy_file(backup_, restore, std::filesystem::copy_options::none)) {
    throw std::runtime_error("Cannot create maintenance restoration file");
  }
  if (Digest(original_) != installed_digest_)
    throw std::runtime_error("Published package changed before restoration; recovery required");
  Replace(restore, original_, baseline_digest_, installed_digest_);
  if (Digest(original_) != baseline_digest_)
    throw std::runtime_error("Restored package digest differs from backup");
  installed_ = false;
}
void ProjectMaintenancePackage::Finish() { cleanup_ = true; }

void ProjectMaintenancePackage::VerifyActivatedProject(const ProjectService& project) const {
  const auto metadata =
      packages_.BuildRuntimeProjectPair(workspace_, QStringLiteral("staged")).second;
  if (Metadata(metadata).at("project_uuid") != project.GetProjectUUID()) {
    throw std::runtime_error("Activated project identity differs from the verified backup");
  }
  if (Digest(original_) != (installed_ ? installed_digest_ : baseline_digest_)) {
    throw std::runtime_error("Project package changed during activation; recovery required");
  }
}
}  // namespace alcedo
