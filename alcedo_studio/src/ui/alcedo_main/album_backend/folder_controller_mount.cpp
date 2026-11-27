//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// The mounted folder part of FolderController (Mounted Folder Plan, MF6 and MF8): the mount
// fields of the rows, the stored missing state of the tiles, and the mount commands of the
// Collections panel, the image menu, and automation.

#include <QDesktopServices>
#include <QVariantMap>
#include <algorithm>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

#include "app/image_pool_service.hpp"
#include "app/pipeline_service.hpp"
#include "image/image.hpp"
#include "ui/alcedo_main/album_backend/folder_controller.hpp"
#include "ui/alcedo_main/album_backend/import_export.hpp"
#include "ui/alcedo_main/album_backend/library_module.hpp"
#include "ui/alcedo_main/album_backend/path_utils.hpp"
#include "ui/alcedo_main/album_backend/project_module.hpp"
#include "ui/alcedo_main/album_backend/ui_status_sink.hpp"

namespace alcedo::ui {

#define PL_TEXT(text, ...)                     \
  i18n::MakeLocalizedText(ALCEDO_I18N_CONTEXT, \
                          QT_TRANSLATE_NOOP(ALCEDO_I18N_CONTEXT, text) __VA_OPT__(, ) __VA_ARGS__)

namespace {

auto ToPath(const QUrl& url) -> std::optional<std::filesystem::path> {
  return album_util::InputToPath(url.isLocalFile() ? url.toLocalFile() : url.toString());
}

}  // namespace

auto MountCommandErrorText(std::exception_ptr error) -> QString {
  if (!error) {
    return {};
  }
  try {
    std::rethrow_exception(error);
  } catch (const MountOperationError& e) {
    switch (e.Code()) {
      case MountError::kNestedMount:
        return Tr("The folder is inside a mounted folder or holds one.");
      case MountError::kDirectoryUnreachable:
        return Tr("The folder cannot be reached.");
      case MountError::kUnknownMount:
        return Tr("The mounted folder no longer exists.");
      case MountError::kStorageFailure:
        return Tr("The change could not be stored.");
      case MountError::kPhotoIsNotMissing:
        return Tr("The photo is not missing.");
      case MountError::kFileOutsideMount:
        return Tr("The file is outside the mounted folder.");
      case MountError::kFileIsNotListed:
        return Tr("The mounted folder does not show this file.");
      case MountError::kFileHasPhoto:
        return Tr("Another photo uses this file.");
      case MountError::kFileUnreadable:
        return Tr("The file cannot be read.");
    }
    return QString::fromUtf8(e.what());
  } catch (const std::exception& e) {
    return QString::fromUtf8(e.what());
  } catch (...) {
    return Tr("The change could not be stored.");
  }
}

// ── Row fields and cached mount states ──────────────────────────────────────

void FolderController::AddMountFields(const ExistingFolderEntry& folder, QVariantMap& row) const {
  const MountSummary* summary =
      folder.mount_id_.has_value() ? FindMountSummary(*folder.mount_id_) : nullptr;
  const bool root = summary != nullptr && summary->record.root_folder_id == folder.element_id_;
  row.insert(QStringLiteral("mountId"), static_cast<uint>(folder.mount_id_.value_or(0)));
  row.insert(QStringLiteral("mountRoot"), root);
  row.insert(QStringLiteral("mountOffline"),
             summary != nullptr && summary->reachability == MountReachability::kOffline);
  row.insert(QStringLiteral("mountMissingCount"),
             root ? static_cast<qulonglong>(summary->missing_count) : 0ULL);
  row.insert(QStringLiteral("structureEditable"), !folder.mount_id_.has_value());
  // The Disk section groups the mounts by volume (#1078): the stored identity, else the mount
  // point of this session, else the root of the directory path.
  QString volume_key;
  QString volume_name;
  QString mount_point;
  QString volume_root;
  if (summary != nullptr) {
    const auto& record = summary->record;
    volume_name        = QString::fromStdString(record.volume_name);
    mount_point        = album_util::MountPointDisplay(summary->volume_mount_point);
    volume_root        = album_util::MountPointDisplay(record.root_path.root_path());
    volume_key         = !record.volume_identity.empty()
                             ? QString::fromStdString(record.volume_identity)
                             : (!mount_point.isEmpty() ? mount_point : volume_root);
  }
  row.insert(QStringLiteral("mountVolumeKey"), volume_key);
  row.insert(QStringLiteral("mountVolumeName"), volume_name);
  row.insert(QStringLiteral("mountPoint"), mount_point);
  row.insert(QStringLiteral("mountVolumeRoot"), volume_root);
}

auto FolderController::FindMountSummary(mount_id_t mount_id) const -> const MountSummary* {
  const auto it = std::find_if(
      mount_summaries_.begin(), mount_summaries_.end(),
      [mount_id](const MountSummary& summary) { return summary.record.id == mount_id; });
  return it != mount_summaries_.end() ? &*it : nullptr;
}

void FolderController::ApplyMountSummaries(std::vector<MountSummary> summaries) {
  mount_summaries_ = std::move(summaries);
  present_mounted_ids_.clear();
  missing_mounted_ids_.clear();
  for (const auto& summary : mount_summaries_) {
    present_mounted_ids_.insert(summary.present_element_ids.begin(),
                                summary.present_element_ids.end());
    missing_mounted_ids_.insert(summary.missing_element_ids.begin(),
                                summary.missing_element_ids.end());
  }
  if (library_ != nullptr) {
    library_->thumbs().SetStoredMissingSources(missing_mounted_ids_);
  }
  emit MountStateChanged();
}

auto FolderController::CurrentFolderMounted() const -> bool {
  const auto it = nodes_by_path_.find(PathKey(current_folder_path_));
  return it != nodes_by_path_.end() && it->second.mount_id_.has_value();
}

auto FolderController::MountOfRow(uint folderUiId) const -> std::optional<mount_id_t> {
  const auto key = path_key_by_ui_id_.find(folderUiId);
  if (key != path_key_by_ui_id_.end()) {
    const auto node = nodes_by_path_.find(key->second);
    if (node != nodes_by_path_.end() && node->second.mount_id_.has_value()) {
      return node->second.mount_id_;
    }
  }
  status_->SetTaskState(PL_TEXT("The collection is not a mounted folder."), 0, false);
  return std::nullopt;
}

auto FolderController::MountedDirectoryOfRow(uint folderUiId) const
    -> std::optional<std::filesystem::path> {
  const auto  mount_id = MountOfRow(folderUiId);
  const auto  row_path = TryGetPathForUiId(folderUiId);
  const auto* summary  = mount_id.has_value() ? FindMountSummary(*mount_id) : nullptr;
  if (summary == nullptr || !row_path.has_value()) {
    return std::nullopt;
  }
  for (const auto& [key, node] : nodes_by_path_) {
    (void)key;
    if (node.element_id_ != summary->record.root_folder_id) {
      continue;
    }
    // The folders below the mount root have the names of their directories.
    const auto relative = row_path->lexically_relative(node.folder_path_);
    if (relative.empty() || *relative.begin() == "..") {
      return std::nullopt;
    }
    return relative == "." ? summary->record.root_path
                           : (summary->record.root_path / relative).lexically_normal();
  }
  return std::nullopt;
}

auto FolderController::IsMissingMountedPhoto(uint elementId) const -> bool {
  return missing_mounted_ids_.contains(static_cast<sl_element_id_t>(elementId));
}

auto FolderController::IsMountedPhoto(uint elementId) const -> bool {
  const auto element_id = static_cast<sl_element_id_t>(elementId);
  return present_mounted_ids_.contains(element_id) || missing_mounted_ids_.contains(element_id);
}

auto FolderController::MissingPhotoLocateFolder(uint elementId) const -> QUrl {
  const auto  element_id = static_cast<sl_element_id_t>(elementId);
  // `Image.image_path` is the last location of the file; the album row holds the library path.
  const auto* item       = library_ != nullptr ? library_->FindAlbumItem(element_id) : nullptr;
  const auto  project    = project_->handler().project();
  if (item != nullptr && item->image_id != 0 && project && project->GetImagePoolService()) {
    std::filesystem::path image_path;
    try {
      image_path = project->GetImagePoolService()->Read<std::filesystem::path>(
          item->image_id, [](const std::shared_ptr<Image>& image) { return image->image_path_; });
    } catch (const std::exception&) {
    }
    std::error_code ec;
    if (!image_path.empty() && std::filesystem::is_directory(image_path.parent_path(), ec)) {
      return QUrl::fromLocalFile(album_util::PathToQString(image_path.parent_path()));
    }
  }
  for (const auto& summary : mount_summaries_) {
    if (std::binary_search(summary.missing_element_ids.begin(), summary.missing_element_ids.end(),
                           element_id)) {
      return QUrl::fromLocalFile(album_util::PathToQString(summary.record.root_path));
    }
  }
  return {};
}

// ── Command entries ─────────────────────────────────────────────────────────

auto FolderController::MountCommandRefusal() const -> QString {
  auto& ph = project_->handler();
  if (ph.project_loading()) {
    return Tr("Project is loading. Please wait.");
  }
  if (!ph.project() || !ph.project()->GetMountService()) {
    return Tr("No project is loaded.");
  }
  if (mount_scans_ == nullptr || !mount_scans_->IsStarted()) {
    return Tr("Mounted folders are not available until the project is open.");
  }
  auto* ie = import_export_;
  if (ie && ie->current_import_job() && !ie->current_import_job()->IsCancelationAcked()) {
    return Tr("Cannot change a mounted folder while import is running.");
  }
  return {};
}

auto FolderController::StartCreateMount(const std::filesystem::path& directory,
                                        std::function<void(std::exception_ptr, mount_id_t)> done,
                                        MountScanEndedCallback first_scan_ended) -> QString {
  if (auto refusal = MountCommandRefusal(); !refusal.isEmpty()) {
    return refusal;
  }
  auto mounts  = project_->handler().project()->GetMountService();
  auto created = std::make_shared<mount_id_t>(0);
  mount_scans_->RunMountOperation(
      [mounts, directory, created] { *created = mounts->CreateMount(directory); },
      [this, created, done = std::move(done),
       first_scan_ended = std::move(first_scan_ended)](std::exception_ptr error) mutable {
        if (!error) {
          mount_scans_->ReloadMounts();
          library_->ReloadFolderTree();
          (void)SaveProjectState();
          mount_scans_->RequestScan(*created, MountScanTrigger::kSynchronize,
                                    std::move(first_scan_ended));
        }
        if (done) {
          done(error, *created);
        }
      });
  return {};
}

auto FolderController::StartSynchronize(mount_id_t mount_id, MountScanEndedCallback ended)
    -> QString {
  if (auto refusal = MountCommandRefusal(); !refusal.isEmpty()) {
    return refusal;
  }
  if (!mount_scans_->RequestScan(mount_id, MountScanTrigger::kSynchronize, std::move(ended))) {
    return Tr("The mounted folder no longer exists.");
  }
  return {};
}

auto FolderController::StartUnmount(mount_id_t mount_id, MountCommandDone done) -> QString {
  if (auto refusal = MountCommandRefusal(); !refusal.isEmpty()) {
    return refusal;
  }
  auto mounts = project_->handler().project()->GetMountService();
  mount_scans_->RunMountOperation([mounts, mount_id] { mounts->RemoveMount(mount_id); },
                                  [this, done = std::move(done)](std::exception_ptr error) {
                                    if (!error) {
                                      mount_scans_->ReloadMounts();
                                      library_->ReloadFolderTree();
                                      library_->RequestLibraryContentRefresh();
                                      (void)SaveProjectState();
                                    }
                                    if (done) {
                                      done(error);
                                    }
                                  });
  return {};
}

auto FolderController::StartRelocateMountRoot(mount_id_t                   mount_id,
                                              const std::filesystem::path& directory,
                                              MountCommandDone             done,
                                              MountScanEndedCallback       scan_ended) -> QString {
  if (auto refusal = MountCommandRefusal(); !refusal.isEmpty()) {
    return refusal;
  }
  auto mounts = project_->handler().project()->GetMountService();
  mount_scans_->RunMountOperation(
      [mounts, mount_id, directory] { mounts->RelocateRoot(mount_id, directory); },
      [this, mount_id, done = std::move(done),
       scan_ended = std::move(scan_ended)](std::exception_ptr error) mutable {
        if (!error) {
          // The watcher takes the new root; the scan compares the new location.
          mount_scans_->ReloadMounts();
          mount_scans_->RequestScan(mount_id, MountScanTrigger::kSynchronize,
                                    std::move(scan_ended));
          RefreshImageCounts();
          (void)SaveProjectState();
        }
        if (done) {
          done(error);
        }
      });
  return {};
}

auto FolderController::StartLocateMissingPhoto(sl_element_id_t              element_id,
                                               const std::filesystem::path& file,
                                               MountCommandDone             done) -> QString {
  if (auto refusal = MountCommandRefusal(); !refusal.isEmpty()) {
    return refusal;
  }
  auto mounts = project_->handler().project()->GetMountService();
  mount_scans_->RunMountOperation(
      [mounts, element_id, file] { mounts->LocateMissingFile(element_id, file); },
      [this, element_id, done = std::move(done)](std::exception_ptr error) {
        if (!error) {
          if (auto thumbnails = project_->handler().thumbnail_service()) {
            thumbnails->InvalidateThumbnail(element_id);
          }
          // The located file can be in a directory that the last scan did not see.
          library_->ReloadFolderTree();
          library_->RequestLibraryContentRefresh();
          (void)SaveProjectState();
        }
        if (done) {
          done(error);
        }
      });
  return {};
}

auto FolderController::StartRemoveMissingPhotos(
    mount_id_t                                                               mount_id,
    std::function<void(std::exception_ptr, const MountMissingPhotoRemoval&)> done) -> QString {
  if (auto refusal = MountCommandRefusal(); !refusal.isEmpty()) {
    return refusal;
  }
  auto& ph      = project_->handler();
  auto  mounts  = ph.project()->GetMountService();
  auto  removal = std::make_shared<MountMissingPhotoRemoval>();
  mount_scans_->RunMountOperation(
      [mounts, mount_id, removal, pipelines = ph.pipeline_service()] {
        *removal = mounts->RemoveMissingPhotos(mount_id,
                                               [&pipelines](std::span<const sl_element_id_t> ids) {
                                                 if (pipelines) {
                                                   pipelines->DeletePipelines(ids);
                                                 }
                                               });
      },
      [this, removal, done = std::move(done)](std::exception_ptr error) {
        for (std::size_t index = 0; index < removal->element_ids.size(); ++index) {
          library_->thumbs().RemoveThumbnailState(removal->element_ids[index],
                                                  removal->image_ids[index]);
        }
        if (!removal->element_ids.empty()) {
          library_->selection()->Prune(removal->element_ids);
          library_->RequestLibraryContentRefresh();
          RefreshImageCounts();
          (void)SaveProjectState();
        }
        if (done) {
          done(error, *removal);
        }
      });
  return {};
}

// ── Q_INVOKABLE commands of the panel and the image menu ────────────────────

void FolderController::ReportMountCommand(std::exception_ptr         error,
                                          const i18n::LocalizedText& done) {
  if (error) {
    status_->SetTaskState(PL_TEXT("%1", MountCommandErrorText(error)), 0, false);
    return;
  }
  status_->SetTaskState(done, 100, false);
  status_->ScheduleIdleTaskStateReset(1200);
}

void FolderController::MountFolder(const QUrl& directory) {
  const auto directory_path = ToPath(directory);
  if (!directory_path.has_value()) {
    status_->SetTaskState(PL_TEXT("The folder cannot be reached."), 0, false);
    return;
  }
  const auto refusal =
      StartCreateMount(*directory_path, [this](std::exception_ptr error, mount_id_t) {
        ReportMountCommand(error, PL_TEXT("Folder mounted."));
      });
  if (!refusal.isEmpty()) {
    status_->SetTaskState(PL_TEXT("%1", refusal), 0, false);
  }
}

void FolderController::SynchronizeMount(uint folderUiId) {
  const auto mount_id = MountOfRow(folderUiId);
  if (!mount_id.has_value()) {
    return;
  }
  if (const auto refusal = StartSynchronize(*mount_id); !refusal.isEmpty()) {
    status_->SetTaskState(PL_TEXT("%1", refusal), 0, false);
  }
}

void FolderController::Unmount(uint folderUiId) {
  const auto mount_id = MountOfRow(folderUiId);
  if (!mount_id.has_value()) {
    return;
  }
  const auto refusal = StartUnmount(*mount_id, [this](std::exception_ptr error) {
    ReportMountCommand(error, PL_TEXT("Folder unmounted."));
  });
  if (!refusal.isEmpty()) {
    status_->SetTaskState(PL_TEXT("%1", refusal), 0, false);
  }
}

void FolderController::ShowMountedFolderInFileManager(uint folderUiId) {
  const auto      directory = MountedDirectoryOfRow(folderUiId);
  std::error_code ec;
  if (!directory.has_value() || !std::filesystem::is_directory(*directory, ec)) {
    status_->SetTaskState(PL_TEXT("The folder cannot be reached."), 0, false);
    return;
  }
  if (!QDesktopServices::openUrl(QUrl::fromLocalFile(album_util::PathToQString(*directory)))) {
    status_->SetTaskState(PL_TEXT("Failed to open source directory."), 0, false);
  }
}

void FolderController::LocateMountFolder(uint folderUiId, const QUrl& directory) {
  const auto mount_id       = MountOfRow(folderUiId);
  const auto directory_path = ToPath(directory);
  if (!mount_id.has_value()) {
    return;
  }
  if (!directory_path.has_value()) {
    status_->SetTaskState(PL_TEXT("The folder cannot be reached."), 0, false);
    return;
  }
  const auto refusal = StartRelocateMountRoot(
      *mount_id, *directory_path,
      [this](std::exception_ptr error) { ReportMountCommand(error, PL_TEXT("Folder located.")); });
  if (!refusal.isEmpty()) {
    status_->SetTaskState(PL_TEXT("%1", refusal), 0, false);
  }
}

void FolderController::RemoveMissingPhotos(uint folderUiId) {
  const auto mount_id = MountOfRow(folderUiId);
  if (!mount_id.has_value()) {
    return;
  }
  const auto refusal = StartRemoveMissingPhotos(
      *mount_id, [this](std::exception_ptr error, const MountMissingPhotoRemoval& removal) {
        ReportMountCommand(error, PL_TEXT("Removed %1 missing photos.",
                                          static_cast<qulonglong>(removal.element_ids.size())));
      });
  if (!refusal.isEmpty()) {
    status_->SetTaskState(PL_TEXT("%1", refusal), 0, false);
  }
}

void FolderController::LocateMissingPhoto(uint elementId, const QUrl& file) {
  const auto file_path = ToPath(file);
  if (!file_path.has_value()) {
    status_->SetTaskState(PL_TEXT("The file cannot be read."), 0, false);
    return;
  }
  const auto refusal = StartLocateMissingPhoto(
      static_cast<sl_element_id_t>(elementId), *file_path,
      [this](std::exception_ptr error) { ReportMountCommand(error, PL_TEXT("Photo located.")); });
  if (!refusal.isEmpty()) {
    status_->SetTaskState(PL_TEXT("%1", refusal), 0, false);
  }
}

}  // namespace alcedo::ui
