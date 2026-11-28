//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/sleeve_service.hpp"

#include <stdexcept>
#include <unordered_set>
#include <vector>

#include "edit/graph/pipeline_document.hpp"
#include "storage/store/mount/mount_store.hpp"
#include "type/mount_records.hpp"
#include "type/source_file_record.hpp"
#include "utils/logging/log.hpp"
#include "utils/string/convert.hpp"

namespace alcedo {

auto SleeveErrorName(const SleeveError code) -> const char* {
  switch (code) {
    case SleeveError::kMountedFolderStructureIsReadOnly:
      return "mounted_folder_structure_is_read_only";
    case SleeveError::kMountedPhotoIsPresent:
      return "mounted_photo_is_present";
    case SleeveError::kMountedPhotoIsMissing:
      return "mounted_photo_is_missing";
  }
  return "unknown";
}

namespace {

/// Throws SleeveOperationError when @p folder is mounted.
void RequireEditableStructure(const SleeveElement& folder) {
  if (SleeveServiceImpl::CanEditFolderStructure(folder)) {
    return;
  }
  throw SleeveOperationError(SleeveError::kMountedFolderStructureIsReadOnly,
                             "The structure of the mounted folder '" +
                                 conv::ToBytes(folder.element_name_) +
                                 "' is read-only. It follows its disk directory.");
}

/// Throws SleeveOperationError when the folder @p folder_id is mounted. An id that names no
/// element passes: the FileSystem operation that follows reports it.
void RequireEditableFolder(FileSystem& fs, const sl_element_id_t folder_id) {
  if (const auto folder = fs.Get(folder_id)) {
    RequireEditableStructure(*folder);
  }
}

/// The file elements of @p file_ids; an id that names no file is left out.
auto FileElements(FileSystem& fs, std::span<const sl_element_id_t> file_ids)
    -> std::vector<std::shared_ptr<SleeveFile>> {
  std::vector<std::shared_ptr<SleeveFile>> files;
  files.reserve(file_ids.size());
  for (const auto file_id : file_ids) {
    auto element = fs.Get(file_id);
    if (!element || element->type_ != ElementType::FILE) {
      continue;
    }
    files.push_back(std::static_pointer_cast<SleeveFile>(element));
  }
  return files;
}

/// The ids of @p file_ids that name a live mounted file, in the order of @p file_ids.
auto MountedFileIds(FileSystem& fs, std::span<const sl_element_id_t> file_ids)
    -> std::vector<sl_element_id_t> {
  std::vector<sl_element_id_t> mounted_ids;
  for (const auto file_id : file_ids) {
    std::shared_ptr<SleeveElement> file;
    try {
      file = fs.Get(file_id);
    } catch (const std::exception&) {
      continue;
    }
    if (file && file->type_ == ElementType::FILE && file->IsLive() && file->mount_id_.has_value()) {
      mounted_ids.push_back(file_id);
    }
  }
  return mounted_ids;
}

/// Throws SleeveOperationError when a live file of @p file_ids is mounted (P7, MF5 data rules).
/// A mounted file with no source record counts as present: the scan that gave it the mount id
/// found its file, and a failed later batch of that scan can leave the record unwritten. Ids that
/// name no live file pass, as in FileSystem::DeleteFilesEverywhere.
void RequireRemovableFromLibrary(FileSystem& fs, MountStore& mount_store,
                                 std::span<const sl_element_id_t> file_ids) {
  const auto mounted_ids = MountedFileIds(fs, file_ids);
  if (mounted_ids.empty()) {
    return;
  }
  for (const auto file_id : mounted_ids) {
    const auto record = mount_store.ReadSourceFile(file_id);
    if (!record.has_value() || record->state == SourceFileState::kPresent) {
      throw SleeveOperationError(
          SleeveError::kMountedPhotoIsPresent,
          "A mounted photo whose file is present stays in the library. Use the Delete mark.");
    }
  }
  throw SleeveOperationError(
      SleeveError::kMountedPhotoIsMissing,
      "A missing mounted photo leaves the library through Remove Missing Photos of its mount.");
}

auto ElementTypeLabel(const ElementType type) -> const char* {
  switch (type) {
    case ElementType::FILE:
      return "file";
    case ElementType::FOLDER:
      return "folder";
  }
  return "unknown";
}

void LogSyncElement(const char* bucket, const std::shared_ptr<SleeveElement>& element) {
  if (!element) {
    return;
  }
  ALCEDO_LOG_DEBUG(Library) << "SleeveServiceImpl: " << bucket << " "
                            << ElementTypeLabel(element->type_) << " id=" << element->element_id_
                            << " name=" << conv::ToBytes(element->element_name_);
}

void ClonePipelineForDuplicate(Storage& storage_service, sl_element_id_t source_file_id,
                               sl_element_id_t duplicate_file_id) {
  const auto source_document =
      storage_service.GetElementStore().GetPipelineJsonByElementId(source_file_id);
  if (!source_document.has_value()) {
    return;
  }

  const auto document = PipelineDocument::FromJson(*source_document);
  if (!document.Graph().Validate().empty() || !document.Graph().ValidateImageBackbone().empty()) {
    throw std::runtime_error("SleeveService: cannot duplicate an invalid pipeline document");
  }
  storage_service.GetElementStore().UpdatePipelineJsonByElementId(duplicate_file_id,
                                                                    document.ToJson());
}

}  // namespace

SleeveServiceImpl::SleeveServiceImpl(std::shared_ptr<Storage> storage_service,
                                     const std::filesystem::path& db_path, sl_element_id_t start_id)
    : storage_(std::move(storage_service)), db_path_(db_path) {
  if (!storage_) {
    throw std::invalid_argument("Storage is null");
  }
  fs_ = std::make_unique<FileSystem>(db_path_, *storage_, start_id);
  fs_->InitRoot();
}

auto SleeveServiceImpl::Sync() -> SyncResult {
  std::lock_guard<std::mutex> lock(fs_lock_);
  SyncResult                  result = SyncLocked();
  // The end of this fs_lock_ scope, as in Read and Write (SN7).
  fs_->EvictToLimit();
  return result;
}

auto SleeveServiceImpl::SyncLocked() -> SyncResult {
  if (!sync_restore_error_.empty()) {
    return {false, 0, sync_restore_error_};
  }

  const auto dirty = fs_->DirtyElements();
  std::vector<std::shared_ptr<SleeveElement>> inserts;
  std::vector<std::shared_ptr<SleeveElement>> updates;
  std::vector<std::shared_ptr<SleeveElement>> deletes;
  for (const auto& element : dirty) {
    switch (element->SyncState()) {
      case SyncFlag::UNSYNC:
        LogSyncElement("Unsynced", element);
        inserts.push_back(element);
        break;
      case SyncFlag::MODIFIED:
        LogSyncElement("Modified", element);
        updates.push_back(element);
        break;
      case SyncFlag::DELETED:
        LogSyncElement("Deleted", element);
        deletes.push_back(element);
        break;
      case SyncFlag::SYNCED:
        break;
    }
  }

  try {
    storage_->GetElementStore().WriteSync(inserts, updates, deletes);
  } catch (const std::exception& error) {
    try {
      fs_->RestoreAfterFailedSync(dirty);
    } catch (const std::exception& restore_error) {
      sync_restore_error_ =
          std::string(error.what()) + "\nFileSystem restoration failed: " + restore_error.what();
      return {false, 0, sync_restore_error_};
    }
    return {false, 0, error.what()};
  }

  fs_->MarkWritten(dirty);
  return {true, static_cast<uint32_t>(dirty.size()), ""};
}

auto SleeveServiceImpl::CanEditFolderStructure(const SleeveElement& folder) -> bool {
  return !folder.mount_id_.has_value();
}

auto SleeveServiceImpl::CheckFolderStructureEditable(const std::filesystem::path& folder_path)
    -> std::optional<sl_element_id_t> {
  return Read<std::optional<sl_element_id_t>>(
      [&folder_path](FileSystem& fs) -> std::optional<sl_element_id_t> {
        std::shared_ptr<SleeveElement> folder;
        try {
          folder = fs.Get(folder_path, false);
        } catch (const std::runtime_error&) {
          return std::nullopt;
        }
        if (folder && folder->type_ == ElementType::FOLDER) {
          RequireEditableStructure(*folder);
          return folder->element_id_;
        }
        return std::nullopt;
      });
}

auto SleeveServiceImpl::GetCurrentID() const -> sl_element_id_t {
  std::lock_guard<std::mutex> lock(fs_lock_);
  return fs_->GetCurrentID();
}

auto SleeveServiceImpl::ResolveElement(const std::filesystem::path& path)
    -> std::shared_ptr<SleeveElement> {
  return Read<std::shared_ptr<SleeveElement>>(
      [path](FileSystem& fs) { return fs.Get(path, false); });
}

auto SleeveServiceImpl::ResolveFolder(const std::filesystem::path& path)
    -> std::shared_ptr<SleeveFolder> {
  auto element = ResolveElement(path);
  if (!element || element->type_ != ElementType::FOLDER || !element->IsLive()) {
    throw std::runtime_error("SleeveService: Target path is not a folder.");
  }
  return std::static_pointer_cast<SleeveFolder>(element);
}

auto SleeveServiceImpl::ListFolderEntries(const std::filesystem::path& folder_path)
    -> std::vector<std::shared_ptr<SleeveElement>> {
  return Read<std::vector<std::shared_ptr<SleeveElement>>>([folder_path](FileSystem& fs) {
    std::vector<std::shared_ptr<SleeveElement>> entries;
    const auto                                  folder = fs.Get(folder_path, false);
    const auto                                  ids    = fs.ListFolderContent(folder->element_id_);
    entries.reserve(ids.size());
    for (const auto id : ids) {
      auto element = fs.Get(id);
      if (!element || !element->IsLive()) {
        continue;
      }
      entries.push_back(std::move(element));
    }
    return entries;
  });
}

auto SleeveServiceImpl::ListChildFolders(const std::filesystem::path& folder_path)
    -> std::vector<FolderTreeEntry> {
  return Read<std::vector<FolderTreeEntry>>(
      [&folder_path](FileSystem& fs) { return fs.ListChildFolders(folder_path); });
}

auto SleeveServiceImpl::ListFolderTree() -> std::vector<FolderTreeEntry> {
  return Read<std::vector<FolderTreeEntry>>([](FileSystem& fs) { return fs.ListFolderTree(); });
}

auto SleeveServiceImpl::CreateFolder(const std::filesystem::path& parent_path,
                                     const file_name_t&           name)
    -> std::pair<std::shared_ptr<SleeveFolder>, SyncResult> {
  auto result = Write<std::shared_ptr<SleeveFolder>>([parent_path, name](FileSystem& fs) {
    RequireEditableStructure(*fs.Get(parent_path, false));
    auto created = fs.Create(parent_path, name, ElementType::FOLDER);
    if (!created || created->type_ != ElementType::FOLDER) {
      throw std::runtime_error("SleeveService: Failed to create folder.");
    }
    return std::dynamic_pointer_cast<SleeveFolder>(created);
  });
  if (!result.first) {
    throw std::runtime_error("SleeveService: Failed to create folder.");
  }
  return result;
}

auto SleeveServiceImpl::CreateFileInLibrary(const file_name_t& name)
    -> std::pair<std::shared_ptr<SleeveFile>, SyncResult> {
  return Write<std::shared_ptr<SleeveFile>>(
      [name](FileSystem& fs) { return fs.CreateFileInLibrary(name); });
}

auto SleeveServiceImpl::LinkFileToFolder(sl_element_id_t file_id, sl_element_id_t folder_id)
    -> SyncResult {
  return Write<void>([file_id, folder_id](FileSystem& fs) {
    RequireEditableFolder(fs, folder_id);
    fs.LinkFileToFolder(file_id, folder_id);
  });
}

auto SleeveServiceImpl::DeleteFileFromFolder(sl_element_id_t file_id,
                                             sl_element_id_t folder_id) -> SyncResult {
  return Write<void>([file_id, folder_id](FileSystem& fs) {
    RequireEditableFolder(fs, folder_id);
    fs.UnlinkFileFromFolder(file_id, folder_id);
  });
}

auto SleeveServiceImpl::DeleteFilesFromFolder(std::span<const sl_element_id_t> file_ids,
                                              sl_element_id_t                  folder_id)
    -> std::pair<std::vector<std::shared_ptr<SleeveFile>>, SyncResult> {
  return Write<std::vector<std::shared_ptr<SleeveFile>>>(
      [file_ids, folder_id](FileSystem& fs) -> std::vector<std::shared_ptr<SleeveFile>> {
        RequireEditableFolder(fs, folder_id);
        std::vector<std::shared_ptr<SleeveFile>> files;
        files.reserve(file_ids.size());

        const auto removed_ids = fs.UnlinkFilesFromFolder(file_ids, folder_id);
        files.reserve(removed_ids.size());
        for (const auto file_id : removed_ids) {
          auto element = fs.Get(file_id);
          if (!element || element->type_ != ElementType::FILE) {
            continue;
          }
          files.push_back(std::static_pointer_cast<SleeveFile>(element));
        }
        return files;
      });
}

auto SleeveServiceImpl::DeleteFileEverywhere(sl_element_id_t file_id) -> SyncResult {
  return Write<void>([this, file_id](FileSystem& fs) {
    RequireRemovableFromLibrary(fs, storage_->GetMountStore(), std::span(&file_id, 1));
    fs.DeleteFileEverywhere(file_id);
  });
}

auto SleeveServiceImpl::DeleteFilesEverywhere(std::span<const sl_element_id_t> file_ids)
    -> std::pair<std::vector<std::shared_ptr<SleeveFile>>, SyncResult> {
  return Write<std::vector<std::shared_ptr<SleeveFile>>>(
      [this, file_ids](FileSystem& fs) -> std::vector<std::shared_ptr<SleeveFile>> {
        RequireRemovableFromLibrary(fs, storage_->GetMountStore(), file_ids);
        return FileElements(fs, fs.DeleteFilesEverywhere(file_ids));
      });
}

auto SleeveServiceImpl::DeleteFilesEverywhereKeepingMounted(
    std::span<const sl_element_id_t> file_ids) -> std::pair<LibraryRemoval, SyncResult> {
  return Write<LibraryRemoval>([this, file_ids](FileSystem& fs) -> LibraryRemoval {
    LibraryRemoval out;
    out.kept_mounted_ids_ = MountedFileIds(fs, file_ids);
    const std::unordered_set<sl_element_id_t> kept(out.kept_mounted_ids_.begin(),
                                                   out.kept_mounted_ids_.end());
    std::vector<sl_element_id_t>              removable_ids;
    removable_ids.reserve(file_ids.size());
    for (const auto file_id : file_ids) {
      if (!kept.contains(file_id)) {
        removable_ids.push_back(file_id);
      }
    }
    if (removable_ids.empty() && !kept.empty()) {
      // Only mounted photos: the request is refused with the reason of the strict delete.
      RequireRemovableFromLibrary(fs, storage_->GetMountStore(), file_ids);
    }
    out.deleted_files_ = FileElements(fs, fs.DeleteFilesEverywhere(removable_ids));
    return out;
  });
}

auto SleeveServiceImpl::DuplicateFileToFolder(sl_element_id_t file_id,
                                              sl_element_id_t folder_id)
    -> std::pair<std::shared_ptr<SleeveFile>, SyncResult> {
  std::lock_guard<std::mutex> lock(fs_lock_);

  if (!sync_restore_error_.empty()) {
    throw std::runtime_error(sync_restore_error_);
  }

  RequireEditableFolder(*fs_, folder_id);
  auto       duplicated  = fs_->DuplicateFileToFolder(file_id, folder_id);
  SyncResult sync_result = SyncLocked();
  if (duplicated && sync_result.success_) {
    try {
      ClonePipelineForDuplicate(*storage_, file_id, duplicated->element_id_);
    } catch (const std::exception& e) {
      sync_result.success_ = false;
      sync_result.message_ = e.what();
    }
  }

  // The end of this fs_lock_ scope, as in Read and Write (SN7).
  fs_->EvictToLimit();
  return {duplicated, sync_result};
}

auto SleeveServiceImpl::DeletePath(const std::filesystem::path& target_path) -> SyncResult {
  return Write<void>([target_path](FileSystem& fs) {
    // A path names a folder only (a file name is a label); a file leaves by its id.
    RequireEditableStructure(*fs.Get(target_path, false));
    fs.Delete(target_path);
  });
}

auto SleeveServiceImpl::DeleteElement(sl_element_id_t target_id) -> SyncResult {
  return Write<void>([this, target_id](FileSystem& fs) {
    // FileSystem::Delete removes a file from the library.
    if (const auto target = fs.Get(target_id)) {
      if (target->type_ == ElementType::FOLDER) {
        RequireEditableStructure(*target);
      } else {
        RequireRemovableFromLibrary(fs, storage_->GetMountStore(), std::span(&target_id, 1));
      }
    }
    fs.Delete(target_id);
  });
}

auto SleeveServiceImpl::RenameFolder(const std::filesystem::path& folder_path,
                                     const file_name_t& new_name) -> SyncResult {
  return Write<void>([folder_path, new_name](FileSystem& fs) {
    RequireEditableStructure(*fs.Get(folder_path, false));
    fs.RenameFolder(folder_path, new_name);
  });
}

auto SleeveServiceImpl::ReadMountFolders(sl_element_id_t root_folder_id, mount_id_t mount_id)
    -> std::vector<MountTreeFolder> {
  return Read<std::vector<MountTreeFolder>>([root_folder_id, mount_id](FileSystem& fs) {
    return alcedo::ReadMountFolders(fs, root_folder_id, mount_id);
  });
}

auto SleeveServiceImpl::ReadMountTree(sl_element_id_t root_folder_id, mount_id_t mount_id)
    -> MountTree {
  return Read<MountTree>([root_folder_id, mount_id](FileSystem& fs) {
    return alcedo::ReadMountTree(fs, root_folder_id, mount_id);
  });
}

auto SleeveServiceImpl::ApplyMountTreeChanges(const MountTreeChanges& changes) -> SyncResult {
  return Write<void>([&changes](FileSystem& fs) { alcedo::ApplyMountTreeChanges(fs, changes); });
}

auto SleeveServiceImpl::CreateMountRootFolder(const file_name_t& name, mount_id_t mount_id)
    -> std::pair<std::shared_ptr<SleeveFolder>, SyncResult> {
  return Write<std::shared_ptr<SleeveFolder>>([&name, mount_id](FileSystem& fs) {
    return alcedo::CreateMountRootFolder(fs, name, mount_id);
  });
}

auto SleeveServiceImpl::RemoveMountTree(sl_element_id_t root_folder_id, mount_id_t mount_id,
                                        std::span<const sl_element_id_t> file_ids) -> SyncResult {
  return Write<void>([root_folder_id, mount_id, file_ids](FileSystem& fs) {
    alcedo::RemoveMountTree(fs, root_folder_id, mount_id, file_ids);
  });
}

auto SleeveServiceImpl::DeleteMissingMountedFiles(mount_id_t                       mount_id,
                                                  std::span<const sl_element_id_t> file_ids)
    -> std::pair<std::vector<std::shared_ptr<SleeveFile>>, SyncResult> {
  return Write<std::vector<std::shared_ptr<SleeveFile>>>(
      [this, mount_id, file_ids](FileSystem& fs) -> std::vector<std::shared_ptr<SleeveFile>> {
        auto& mounts = storage_->GetMountStore();
        for (const auto file_id : file_ids) {
          const auto file   = fs.Get(file_id);
          const auto record = mounts.ReadSourceFile(file_id);
          if (!file || file->type_ != ElementType::FILE || !file->IsLive() ||
              file->mount_id_ != mount_id || !record.has_value() ||
              record->state != SourceFileState::kMissing) {
            throw std::invalid_argument("SleeveServiceImpl: the element " +
                                        std::to_string(file_id) +
                                        " is not a missing photo of the mount");
          }
        }
        std::vector<std::shared_ptr<SleeveFile>> files;
        for (const auto file_id : fs.DeleteFilesEverywhere(file_ids)) {
          auto element = fs.Get(file_id);
          if (element && element->type_ == ElementType::FILE) {
            files.push_back(std::static_pointer_cast<SleeveFile>(element));
          }
        }
        return files;
      });
}

auto SleeveServiceImpl::MoveFolder(const std::filesystem::path& folder_path,
                                   const std::filesystem::path& new_parent_path) -> SyncResult {
  return Write<void>([folder_path, new_parent_path](FileSystem& fs) {
    RequireEditableStructure(*fs.Get(folder_path, false));
    RequireEditableStructure(*fs.Get(new_parent_path, false));
    fs.MoveFolder(folder_path, new_parent_path);
  });
}
};  // namespace alcedo
