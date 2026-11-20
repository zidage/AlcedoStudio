//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/sleeve_service.hpp"

#include <algorithm>
#include <stdexcept>
#include <unordered_set>

#include "edit/graph/pipeline_document.hpp"
#include "storage/store/mount/mount_store.hpp"
#include "type/mount_records.hpp"
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

/// Throws SleeveOperationError when a live file of @p file_ids is mounted (P7, MF5 data rules).
/// A mounted file with no source record counts as present: the scan that gave it the mount id
/// found its file, and a failed later batch of that scan can leave the record unwritten. Ids that
/// name no live file pass, as in FileSystem::DeleteFilesEverywhere.
void RequireRemovableFromLibrary(FileSystem& fs, MountStore& mount_store,
                                 std::span<const sl_element_id_t> file_ids) {
  std::vector<sl_element_id_t> mounted_ids;
  for (const auto file_id : file_ids) {
    std::shared_ptr<SleeveElement> file;
    try {
      file = fs.Get(file_id);
    } catch (const std::exception&) {
      continue;
    }
    if (file && file->type_ == ElementType::FILE && file->sync_flag_ != SyncFlag::DELETED &&
        file->mount_id_.has_value()) {
      mounted_ids.push_back(file_id);
    }
  }
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

void RemoveIdsFrom(std::vector<std::shared_ptr<SleeveElement>>& elements,
                   const std::unordered_set<sl_element_id_t>&   blocked_ids) {
  elements.erase(std::remove_if(elements.begin(), elements.end(),
                                [&blocked_ids](const std::shared_ptr<SleeveElement>& element) {
                                  return !element || blocked_ids.contains(element->element_id_);
                                }),
                 elements.end());
}

auto CollectIds(const std::vector<std::shared_ptr<SleeveElement>>& elements)
    -> std::unordered_set<sl_element_id_t> {
  std::unordered_set<sl_element_id_t> ids;
  ids.reserve(elements.size());
  for (const auto& element : elements) {
    if (!element) {
      continue;
    }
    ids.insert(element->element_id_);
  }
  return ids;
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
  SyncResult result{true, 0, ""};
  try {
    auto&      element_ctrl      = storage_->GetElementStore();
    auto       modified_elements = fs_->GetModifiedElements();
    auto       unsynced_elements = fs_->GetUnsyncedElements();
    auto       garbage_elements  = fs_->GetDeletedElements();

    const auto deleted_ids       = CollectIds(garbage_elements);
    RemoveIdsFrom(unsynced_elements, deleted_ids);

    auto       skip_modified_ids = deleted_ids;
    const auto unsynced_ids      = CollectIds(unsynced_elements);
    skip_modified_ids.insert(unsynced_ids.begin(), unsynced_ids.end());
    RemoveIdsFrom(modified_elements, skip_modified_ids);

    // Sync unsynced elements first, batched into a single transaction instead of
    // one autocommit transaction per element.
    for (auto& element : unsynced_elements) {
      LogSyncElement("Unsynced", element);
    }
    element_ctrl.AddElements(unsynced_elements);
    result.elements_synced_ += static_cast<uint32_t>(unsynced_elements.size());

    // Then sync modified elements, also batched into a single transaction.
    for (auto& element : modified_elements) {
      LogSyncElement("Modified", element);
    }
    element_ctrl.UpdateElements(modified_elements);
    result.elements_synced_ += static_cast<uint32_t>(modified_elements.size());

    // Finally, delete the deleted elements.
    // TODO: This should be done periodically instead of every sync.
    for (auto& element : garbage_elements) {
      LogSyncElement("Deleted", element);
    }
    element_ctrl.RemoveElements(garbage_elements);
    result.elements_synced_ += static_cast<uint32_t>(garbage_elements.size());
    // Perform garbage collection in the storage
    // The same goes to here, this should be done periodically
    fs_->GarbageCollect();
  } catch (std::exception& e) {
    result.success_ = false;
    result.message_ = e.what();
  }
  return result;
}

auto SleeveServiceImpl::CanEditFolderStructure(const SleeveElement& folder) -> bool {
  return !folder.mount_id_.has_value();
}

void SleeveServiceImpl::CheckFolderStructureEditable(const std::filesystem::path& folder_path) {
  Read<void>([&folder_path](FileSystem& fs) {
    std::shared_ptr<SleeveElement> folder;
    try {
      folder = fs.Get(folder_path, false);
    } catch (const std::runtime_error&) {
      return;
    }
    if (folder && folder->type_ == ElementType::FOLDER) {
      RequireEditableStructure(*folder);
    }
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
  if (!element || element->type_ != ElementType::FOLDER ||
      element->sync_flag_ == SyncFlag::DELETED) {
    throw std::runtime_error("SleeveService: Target path is not a folder.");
  }
  return std::static_pointer_cast<SleeveFolder>(element);
}

auto SleeveServiceImpl::ResolveFile(const std::filesystem::path& path)
    -> std::shared_ptr<SleeveFile> {
  auto element = ResolveElement(path);
  if (!element || element->type_ != ElementType::FILE || element->sync_flag_ == SyncFlag::DELETED) {
    throw std::runtime_error("SleeveService: Target path is not a file.");
  }
  auto file = std::dynamic_pointer_cast<SleeveFile>(element);
  if (!file) {
    throw std::runtime_error("SleeveService: Failed to resolve file pointer.");
  }
  return file;
}

auto SleeveServiceImpl::ListFolderEntries(const std::filesystem::path& folder_path)
    -> std::vector<std::shared_ptr<SleeveElement>> {
  return Read<std::vector<std::shared_ptr<SleeveElement>>>([folder_path](FileSystem& fs) {
    std::vector<std::shared_ptr<SleeveElement>> entries;
    const auto                                  ids = fs.ListFolderContent(folder_path, false);
    entries.reserve(ids.size());
    for (const auto id : ids) {
      auto element = fs.Get(id);
      if (!element || element->sync_flag_ == SyncFlag::DELETED) {
        continue;
      }
      entries.push_back(std::move(element));
    }
    return entries;
  });
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
        std::vector<std::shared_ptr<SleeveFile>> files;
        files.reserve(file_ids.size());

        const auto deleted_ids = fs.DeleteFilesEverywhere(file_ids);
        files.reserve(deleted_ids.size());
        for (const auto file_id : deleted_ids) {
          auto element = fs.Get(file_id);
          if (!element || element->type_ != ElementType::FILE) {
            continue;
          }
          files.push_back(std::static_pointer_cast<SleeveFile>(element));
        }
        return files;
      });
}

auto SleeveServiceImpl::DuplicateFileToFolder(sl_element_id_t file_id,
                                              sl_element_id_t folder_id)
    -> std::pair<std::shared_ptr<SleeveFile>, SyncResult> {
  std::lock_guard<std::mutex> lock(fs_lock_);

  RequireEditableFolder(*fs_, folder_id);
  auto       duplicated  = fs_->DuplicateFileToFolder(file_id, folder_id);
  SyncResult sync_result = Sync();
  if (!duplicated || !sync_result.success_) {
    return {duplicated, sync_result};
  }

  try {
    ClonePipelineForDuplicate(*storage_, file_id, duplicated->element_id_);
  } catch (const std::exception& e) {
    sync_result.success_ = false;
    sync_result.message_ = e.what();
  }

  return {duplicated, sync_result};
}

auto SleeveServiceImpl::DeletePath(const std::filesystem::path& target_path) -> SyncResult {
  return Write<void>([this, target_path](FileSystem& fs) {
    // FileSystem::Delete removes a file of the root from the library and unlinks a file of any
    // other folder from that folder.
    const auto target = fs.Get(target_path, false);
    if (target->type_ == ElementType::FOLDER) {
      RequireEditableStructure(*target);
    } else if (const auto parent = fs.Get(target_path.parent_path(), false);
               parent->element_id_ == 0) {
      RequireRemovableFromLibrary(fs, storage_->GetMountStore(),
                                  std::span(&target->element_id_, 1));
    } else {
      RequireEditableStructure(*parent);
    }
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

auto SleeveServiceImpl::ReadMountTree(sl_element_id_t root_folder_id, mount_id_t mount_id)
    -> MountTree {
  return Read<MountTree>([root_folder_id, mount_id](FileSystem& fs) {
    return alcedo::ReadMountTree(fs, root_folder_id, mount_id);
  });
}

auto SleeveServiceImpl::ApplyMountTreeChanges(const MountTreeChanges& changes) -> SyncResult {
  return Write<void>([&changes](FileSystem& fs) { alcedo::ApplyMountTreeChanges(fs, changes); });
}

auto SleeveServiceImpl::CreateMountRootFolder(sl_element_id_t parent_id, const file_name_t& name,
                                              mount_id_t mount_id)
    -> std::pair<std::shared_ptr<SleeveFolder>, SyncResult> {
  return Write<std::shared_ptr<SleeveFolder>>([parent_id, &name, mount_id](FileSystem& fs) {
    return alcedo::CreateMountRootFolder(fs, parent_id, name, mount_id);
  });
}

auto SleeveServiceImpl::RemoveMountTree(sl_element_id_t root_folder_id, mount_id_t mount_id,
                                        std::span<const sl_element_id_t> file_ids) -> SyncResult {
  return Write<void>([root_folder_id, mount_id, file_ids](FileSystem& fs) {
    alcedo::RemoveMountTree(fs, root_folder_id, mount_id, file_ids);
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
