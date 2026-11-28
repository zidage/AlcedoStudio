//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "sleeve/sleeve_filesystem.hpp"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <span>
#include <stdexcept>
#include <unordered_set>
#include <vector>

#include "sleeve/sleeve_element/sleeve_element.hpp"
#include "sleeve/sleeve_element/sleeve_element_factory.hpp"
#include "sleeve/sleeve_element/sleeve_folder.hpp"
#include "utils/string/convert.hpp"

namespace alcedo {
namespace {
auto IsRootPath(const std::filesystem::path& path) -> bool {
  const auto normalized = path.lexically_normal();
  return normalized.empty() || normalized == std::filesystem::path{L"/"} ||
         normalized == std::filesystem::path{L"."};
}
}  // namespace

FileSystem::FileSystem(std::filesystem::path db_path, Storage& storage_service,
                       sl_element_id_t start_id)
    : id_gen_(start_id),
      db_path_(db_path),
      storage_service_(storage_service),
      storage_handler_(storage_service_.GetElementStore(), storage_service_.GetFolderEntryStore(),
                       nodes_),
      resolver_(storage_handler_, id_gen_) {
  root_ = nullptr;
}

auto FileSystem::InitRoot() -> bool {
  if (root_ != nullptr) {
    throw std::runtime_error("Filesystem: root has already been initialized");
  }
  // root's id is always 0
  std::shared_ptr<SleeveElement> root;
  try {
    root = storage_service_.GetElementStore().GetElementById(0);
  } catch (std::exception& e) {
    root = SleeveElementFactory::CreateElement(ElementType::FOLDER, 0, L"");
  }
  nodes_.Insert(root);
  root_ = std::static_pointer_cast<SleeveFolder>(root);
  if (root_->SyncState() != SyncFlag::UNSYNC) {
    storage_handler_.LoadFolderTree();
  }
  resolver_.SetRoot(root_);
  return true;
}

auto FileSystem::Create(std::filesystem::path dest, std::wstring filename, ElementType type)
    -> std::shared_ptr<SleeveElement> {
  auto dest_element = resolver_.ResolveForWrite(dest);
  if (dest_element->type_ != ElementType::FOLDER) {
    throw std::runtime_error("Filesystem: Cannot create element under a file");
  }
  auto dest_folder = std::static_pointer_cast<SleeveFolder>(dest_element);
  if (type == ElementType::FOLDER) {
    while (dest_folder->Contains(filename)) {
      filename = filename + L"@";
    }
  }
  auto new_id      = id_gen_.GenerateID();
  auto new_element = SleeveElementFactory::CreateElement(type, new_id, filename);
  nodes_.Insert(new_element);
  if (type == ElementType::FOLDER) {
    dest_folder->AddElementToMap(new_element);
    resolver_.InvalidateAll();
    return new_element;
  }
  // Each file has one link in the library root (D2), which holds the stored reference count of
  // the file.
  new_element->IncrementRefCount();
  root_->RecordFileLinked(new_id);
  if (dest_folder != root_) {
    dest_folder->RecordFileLinked(new_id);
  }
  return new_element;
}

auto FileSystem::CreateFileInLibrary(file_name_t name) -> std::shared_ptr<SleeveFile> {
  auto element = Create(L"", std::move(name), ElementType::FILE);
  return std::static_pointer_cast<SleeveFile>(element);
}

void FileSystem::LinkFileToFolder(sl_element_id_t file_id, sl_element_id_t folder_id) {
  LiveFile(file_id);
  LiveFolder(folder_id)->RecordFileLinked(file_id);
}

void FileSystem::UnlinkFileFromFolder(sl_element_id_t file_id, sl_element_id_t folder_id) {
  if (folder_id == 0) {
    throw std::runtime_error("Filesystem: Root file removal must use DeleteFileEverywhere");
  }
  LiveFile(file_id);
  auto folder = LiveFolder(folder_id);
  if (!storage_handler_.HeldFiles(*folder, std::span(&file_id, 1)).contains(file_id)) {
    throw std::runtime_error("Filesystem: File is not a member of the folder");
  }
  folder->RecordFileUnlinked(file_id);
}

auto FileSystem::UnlinkFilesFromFolder(std::span<const sl_element_id_t> file_ids,
                                       sl_element_id_t                  folder_id)
    -> std::vector<sl_element_id_t> {
  if (folder_id == 0) {
    throw std::runtime_error("Filesystem: Root file removal must use DeleteFilesEverywhere");
  }
  auto                                folder = LiveFolder(folder_id);
  // One read for the batch, before any change: a failed read changes nothing. The read gives
  // only live files, so no file element loads.
  const auto                          held   = storage_handler_.HeldFiles(*folder, file_ids);
  std::vector<sl_element_id_t>        removed_ids;
  std::unordered_set<sl_element_id_t> removed;
  for (const auto file_id : file_ids) {
    if (held.contains(file_id) && removed.insert(file_id).second) {
      folder->RecordFileUnlinked(file_id);
      removed_ids.push_back(file_id);
    }
  }
  return removed_ids;
}

auto FileSystem::LinkFilesToFolder(std::span<const sl_element_id_t> file_ids,
                                   sl_element_id_t folder_id) -> std::vector<sl_element_id_t> {
  auto       folder     = LiveFolder(folder_id);
  const auto linked_ids = storage_handler_.LiveFileIds(*root_, file_ids);
  for (const auto file_id : linked_ids) {
    folder->RecordFileLinked(file_id);
  }
  return linked_ids;
}

auto FileSystem::DuplicateFileToFolder(sl_element_id_t file_id, sl_element_id_t folder_id)
    -> std::shared_ptr<SleeveFile> {
  const auto source_file = std::static_pointer_cast<SleeveFile>(LiveFile(file_id));
  auto       folder      = LiveFolder(folder_id);

  auto duplicated = std::static_pointer_cast<SleeveFile>(source_file->Copy(id_gen_.GenerateID()));
  duplicated->image_id_ = source_file->image_id_;
  // The duplicate is a new photo that no directory holds, so it is an ordinary element.
  duplicated->mount_id_.reset();
  // The root link holds the stored reference count of the file, as in Create.
  duplicated->IncrementRefCount();
  nodes_.Insert(duplicated);
  root_->RecordFileLinked(duplicated->element_id_);
  if (folder != root_) {
    folder->RecordFileLinked(duplicated->element_id_);
  }
  return duplicated;
}

void FileSystem::DeleteFileEverywhere(sl_element_id_t file_id) {
  if (file_id == 0) {
    throw std::runtime_error("Filesystem: root cannot be deleted");
  }
  auto                                      file = LiveFile(file_id);

  // The delete of the element removes every row of the file, so no folder keeps a pending link.
  const std::unordered_set<sl_element_id_t> deleted{file_id};
  nodes_.ForEachFolder([&deleted](SleeveFolder& folder) { folder.DropPendingChanges(deleted); });
  file->SetSyncFlag(SyncFlag::DELETED);
}

auto FileSystem::DeleteFilesEverywhere(std::span<const sl_element_id_t> file_ids)
    -> std::vector<sl_element_id_t> {
  std::vector<sl_element_id_t> deleted_ids;
  deleted_ids.reserve(file_ids.size());
  std::unordered_set<sl_element_id_t> seen;
  seen.reserve(file_ids.size() * 2 + 1);
  std::vector<std::shared_ptr<SleeveElement>> live_files;
  live_files.reserve(file_ids.size());

  for (const auto file_id : file_ids) {
    if (file_id == 0 || !seen.insert(file_id).second) {
      continue;
    }
    std::shared_ptr<SleeveElement> file;
    try {
      file = Get(file_id);
    } catch (...) {
      continue;
    }
    if (!file || file->type_ != ElementType::FILE || !file->IsLive()) {
      continue;
    }
    live_files.push_back(std::move(file));
  }
  if (live_files.empty()) {
    return deleted_ids;
  }

  // One pass over the folders forgets the pending links of every deleted file. A walk of the
  // folders once per file made deleting the rejected files of a large import quadratic.
  std::unordered_set<sl_element_id_t> live_file_ids;
  live_file_ids.reserve(live_files.size() * 2 + 1);
  for (const auto& file : live_files) {
    live_file_ids.insert(file->element_id_);
  }
  nodes_.ForEachFolder(
      [&live_file_ids](SleeveFolder& folder) { folder.DropPendingChanges(live_file_ids); });
  for (const auto& file : live_files) {
    file->SetSyncFlag(SyncFlag::DELETED);
    deleted_ids.push_back(file->element_id_);
  }

  return deleted_ids;
}

auto FileSystem::Get(sl_element_id_t id) -> std::shared_ptr<SleeveElement> {
  return storage_handler_.GetElement(id);
}

auto FileSystem::Get(std::filesystem::path target, bool write) -> std::shared_ptr<SleeveElement> {
  if (write) {
    return resolver_.ResolveForWrite(target);
  }
  return resolver_.Resolve(target);
}

auto FileSystem::ListFolderContent(sl_element_id_t folder_id) -> std::vector<sl_element_id_t> {
  const auto  folder   = LiveFolder(folder_id);
  const auto& children = folder->ListElements();
  auto        members  = std::vector<sl_element_id_t>(children.begin(), children.end());
  const auto  file_ids = storage_handler_.FileIds(*folder);
  members.insert(members.end(), file_ids.begin(), file_ids.end());
  return members;
}

auto FileSystem::ListFileIds(sl_element_id_t folder_id) -> std::vector<sl_element_id_t> {
  return storage_handler_.FileIds(*LiveFolder(folder_id));
}

auto FileSystem::ListMountFileLinks(mount_id_t                       mount_id,
                                    std::span<const sl_element_id_t> folder_ids)
    -> std::vector<FileLink> {
  std::vector<std::shared_ptr<SleeveFolder>> folders;
  folders.reserve(folder_ids.size());
  for (const auto folder_id : folder_ids) {
    folders.push_back(LiveFolder(folder_id));
  }
  return storage_handler_.MountFileLinks(mount_id, folders);
}

void FileSystem::Delete(std::filesystem::path target) {
  if (!target.has_parent_path() || IsRootPath(target)) {
    throw std::runtime_error("Filesystem: root cannot be deleted");
  }
  auto parent           = target.parent_path();
  auto delete_node_name = target.filename();
  if (!resolver_.Contains(parent) || !resolver_.Contains(target)) {
    throw std::runtime_error("Filesystem: Deleting node does not exist");
  }
  auto parent_node    = std::static_pointer_cast<SleeveFolder>(resolver_.Resolve(parent));
  auto delete_node_id = parent_node->GetElementIdByName(delete_node_name.wstring());
  auto delete_node    = storage_handler_.GetElement(delete_node_id.value());

  // Now re-acquire parent_node using write method for folder-tree mutation.
  auto parent_write_node =
      std::static_pointer_cast<SleeveFolder>(resolver_.ResolveForWrite(parent));
  delete_node->DecrementRefCount();

  // Album membership does not own files. Removing a folder only decrements nested folder
  // identities that participate in folder-tree CoW; file children remain live library files.
  // The release goes by id, as in Delete(sl_element_id_t): the path of a child passes through the
  // folder that is now deleted, so it does not resolve.
  if (delete_node->ref_count_ <= 0 && delete_node->type_ == ElementType::FOLDER) {
    ReleaseChildFolders(std::static_pointer_cast<SleeveFolder>(delete_node));
  }

  parent_write_node->RemoveNameFromMap(delete_node_name.wstring());
  // The records of the paths under the target name deleted folders too.
  resolver_.InvalidateAll();
}

void FileSystem::Delete(sl_element_id_t target_id) {
  if (target_id == 0) {
    throw std::runtime_error("Filesystem: root cannot be deleted");
  }

  auto delete_node = Get(target_id);
  if (!delete_node || !delete_node->IsLive()) {
    throw std::runtime_error("Filesystem: Deleting node does not exist");
  }
  if (delete_node->type_ == ElementType::FILE) {
    DeleteFileEverywhere(target_id);
    return;
  }

  const auto remove_from_folder = [this, target_id, &delete_node](
                                      const std::shared_ptr<SleeveFolder>& folder,
                                      const auto&                          self) -> bool {
    const auto children = folder->ListElements();
    for (const auto child_id : children) {
      auto child = storage_handler_.GetElement(child_id);
      if (!child || !child->IsLive()) {
        continue;
      }
      if (child_id == target_id) {
        delete_node->DecrementRefCount();
        if (delete_node->ref_count_ <= 0 && delete_node->type_ == ElementType::FOLDER) {
          ReleaseChildFolders(std::static_pointer_cast<SleeveFolder>(delete_node));
        }
        folder->RemoveNameFromMap(delete_node->element_name_);
        return true;
      }
      if (child->type_ == ElementType::FOLDER &&
          self(std::static_pointer_cast<SleeveFolder>(child), self)) {
        return true;
      }
    }
    return false;
  };

  if (!remove_from_folder(root_, remove_from_folder)) {
    throw std::runtime_error("Filesystem: Deleting node does not exist");
  }
  resolver_.InvalidateAll();
}

void FileSystem::ReleaseChildFolders(const std::shared_ptr<SleeveFolder>& folder) {
  const auto children = folder->ListElements();
  for (const auto child_id : children) {
    auto child = storage_handler_.GetElement(child_id);
    if (!child || !child->IsLive() || child->type_ != ElementType::FOLDER) {
      continue;
    }
    child->DecrementRefCount();
    if (child->ref_count_ <= 0) {
      ReleaseChildFolders(std::static_pointer_cast<SleeveFolder>(child));
    }
  }
}

auto FileSystem::LiveFolder(sl_element_id_t folder_id) -> std::shared_ptr<SleeveFolder> {
  auto element = Get(folder_id);
  if (!element || element->type_ != ElementType::FOLDER || !element->IsLive()) {
    throw std::runtime_error("Filesystem: Specified folder id is not a live folder");
  }
  return std::static_pointer_cast<SleeveFolder>(element);
}

auto FileSystem::LiveFile(sl_element_id_t file_id) -> std::shared_ptr<SleeveElement> {
  auto element = Get(file_id);
  if (!element || element->type_ != ElementType::FILE || !element->IsLive()) {
    throw std::runtime_error("Filesystem: Specified file id is not a live file");
  }
  return element;
}

auto FileSystem::CreateFolderIn(sl_element_id_t parent_id, const file_name_t& name)
    -> std::shared_ptr<SleeveFolder> {
  if (name.empty()) {
    throw std::runtime_error("Filesystem: folder name cannot be empty");
  }
  auto parent = LiveFolder(parent_id);
  if (parent->Contains(name)) {
    throw std::runtime_error("Filesystem: Folder already contains an element with name");
  }
  const auto new_id = id_gen_.GenerateID();
  auto       folder = std::static_pointer_cast<SleeveFolder>(
      SleeveElementFactory::CreateElement(ElementType::FOLDER, new_id, name));
  nodes_.Insert(folder);
  parent->AddElementToMap(folder);
  resolver_.InvalidateAll();
  return folder;
}

void FileSystem::RenameFile(sl_element_id_t file_id, const file_name_t& name) {
  if (name.empty()) {
    throw std::runtime_error("Filesystem: file name cannot be empty");
  }
  auto file = LiveFile(file_id);
  if (file->element_name_ == name) {
    return;
  }
  file->element_name_ = name;
  file->SetLastModifiedTime();
  file->MarkModified();
}

void FileSystem::RemoveFolderFrom(sl_element_id_t parent_id, sl_element_id_t folder_id) {
  if (folder_id == 0) {
    throw std::runtime_error("Filesystem: root cannot be deleted");
  }
  auto parent = LiveFolder(parent_id);
  if (!parent->HoldsChildFolder(folder_id)) {
    throw std::runtime_error("Filesystem: Folder is not a member of the parent folder");
  }
  auto folder = LiveFolder(folder_id);
  folder->DecrementRefCount();
  if (folder->ref_count_ <= 0) {
    ReleaseChildFolders(folder);
  }
  parent->RemoveNameFromMap(folder->element_name_);
  resolver_.InvalidateAll();
}

void FileSystem::RenameFolder(const std::filesystem::path& target, const file_name_t& new_name) {
  if (IsRootPath(target)) {
    throw std::runtime_error("Filesystem: root cannot be renamed");
  }
  if (new_name.empty()) {
    throw std::runtime_error("Filesystem: folder name cannot be empty");
  }
  if (!resolver_.Contains(target, ElementType::FOLDER)) {
    throw std::runtime_error("Filesystem: Renaming folder does not exist");
  }

  auto parent = std::static_pointer_cast<SleeveFolder>(resolver_.ResolveForWrite(target.parent_path()));
  auto folder = resolver_.ResolveForWrite(target);
  if (folder->element_name_ == new_name) {
    return;
  }
  if (const auto existing = parent->GetElementIdByName(new_name);
      existing.has_value() && existing.value() != folder->element_id_) {
    throw std::runtime_error("Filesystem: Folder already contains an element with the new name");
  }

  // The membership edge of the parent is keyed by id, so the remove and the add cancel out in
  // the content change sets; only the element row of the folder records the new name.
  parent->RemoveNameFromMap(folder->element_name_);
  folder->element_name_ = new_name;
  parent->AddElementToMap(folder, true, false);
  folder->SetLastModifiedTime();
  folder->MarkModified();
  resolver_.InvalidateAll();
}

void FileSystem::MoveFolder(const std::filesystem::path& target,
                            const std::filesystem::path& new_parent) {
  if (IsRootPath(target)) {
    throw std::runtime_error("Filesystem: root cannot be moved");
  }
  const auto source = target.lexically_normal();
  const auto destination =
      IsRootPath(new_parent) ? std::filesystem::path{L"/"}.lexically_normal()
                             : new_parent.lexically_normal();
  const auto source_end =
      std::mismatch(source.begin(), source.end(), destination.begin(), destination.end()).first;
  if (source_end == source.end()) {
    throw std::runtime_error("Filesystem: A folder cannot move into itself or into its subfolder");
  }
  if (!resolver_.Contains(source, ElementType::FOLDER) ||
      !resolver_.Contains(destination, ElementType::FOLDER)) {
    throw std::runtime_error("Filesystem: Moving folder or destination folder does not exist");
  }
  if (source.parent_path() == destination) {
    return;
  }

  auto old_parent =
      std::static_pointer_cast<SleeveFolder>(resolver_.ResolveForWrite(source.parent_path()));
  auto folder = resolver_.ResolveForWrite(source);
  auto dest   = std::static_pointer_cast<SleeveFolder>(resolver_.ResolveForWrite(destination));
  if (dest->Contains(folder->element_name_)) {
    throw std::runtime_error("Filesystem: Destination folder already contains an element with name");
  }

  old_parent->RemoveNameFromMap(folder->element_name_);
  dest->AddElementToMap(folder, true, false);
  resolver_.InvalidateAll();
}

void FileSystem::Copy(std::filesystem::path from, std::filesystem::path dest) {
  // Path resolver will do the sanity check
  if (resolver_.IsSubpath(from, dest)) {
    throw std::runtime_error(
        "Filesystem: Target folder cannot be a subfolder of the original folder");
  }
  if (!resolver_.Contains(from) || !resolver_.Contains(dest, ElementType::FOLDER)) {
    throw std::runtime_error("Filesystem: Origin path or destination path does not exist");
  }

  auto from_node = resolver_.Resolve(from);
  auto dest_node = std::static_pointer_cast<SleeveFolder>(resolver_.ResolveForWrite(dest));
  dest_node->AddElementToMap(from_node);
}

auto FileSystem::ApplyFilterToFolder(const std::filesystem::path&       folder_path,
                                     const std::shared_ptr<FilterCombo> filter)
    -> std::vector<std::shared_ptr<SleeveElement>> {
  // TODO: Decouple this into a separate FilterService
  if (!resolver_.Contains(folder_path, ElementType::FOLDER)) {
    throw std::runtime_error("Filesystem: Specified folder does not exist");
  }
  // Here we only need to "read" the folder, so no need to use ResolveForWrite
  std::shared_ptr<SleeveElement> folder_element;
  try {
    folder_element = Get(folder_path, false);
  } catch (std::exception& e) {
    throw std::runtime_error("Filesystem: Specified folder does not exist");
  }

  if (folder_element->type_ != ElementType::FOLDER) {
    throw std::runtime_error("Filesystem: Specified path is not a folder");
  }

  auto folder = std::static_pointer_cast<SleeveFolder>(folder_element);

  std::vector<std::shared_ptr<SleeveElement>> result_elements;
  // First check if the folder has cached index for this filter
  if (folder->HasFilterIndex(filter->filter_id_)) {
    auto& cached_ids = folder->ListElementsByFilter(filter->filter_id_);

    result_elements.reserve(cached_ids.size());
    for (const auto& id : cached_ids) {
      result_elements.push_back(Get(id));
    }
    return result_elements;
  }

  auto result_elements_db = storage_service_.GetElementStore().GetElementsInFolderByFilter(
      filter, folder->element_id_);
  // Just for cache purpose, which will not affect sync status nor anything else
  folder->CreateIndex(result_elements_db, filter->filter_id_);

  // Then return the result from the storage service
  auto& cached_ids = folder->ListElementsByFilter(filter->filter_id_);
  result_elements.reserve(cached_ids.size());
  for (const auto& id : cached_ids) {
    result_elements.push_back(Get(id));
  }
  return result_elements;
}

void FileSystem::MarkWritten(std::span<const std::shared_ptr<SleeveElement>> written) {
  for (const auto& element : written) {
    if (!element->IsLive()) {
      nodes_.Erase(element->element_id_);
      continue;
    }
    if (element->type_ == ElementType::FOLDER) {
      std::static_pointer_cast<SleeveFolder>(element)->MarkContentSynced();
    }
    element->SetSyncFlag(SyncFlag::SYNCED);
  }
}

void FileSystem::RestoreAfterFailedSync(
    std::span<const std::shared_ptr<SleeveElement>> affected) {
  auto  session = storage_service_.OpenSession();
  auto& store   = storage_service_.GetElementStore();
  auto& entries = storage_service_.GetFolderEntryStore();
  resolver_.InvalidateAll();
  for (const auto& element : affected) {
    if (store.RestoreElement(*element)) {
      continue;
    }
    if (element == root_) {
      root_->Clear();
      root_->MarkContentSynced();
      root_->SetSyncFlag(SyncFlag::UNSYNC);
    } else {
      element->SetSyncFlag(SyncFlag::DELETED);
      nodes_.Erase(element->element_id_);
    }
  }
  for (const auto& element : affected) {
    if (element->type_ == ElementType::FOLDER && element->SyncState() == SyncFlag::SYNCED) {
      auto& folder = static_cast<SleeveFolder&>(*element);
      folder.RestoreChildFolders(entries.ReadChildFolderIds(folder.element_id_),
                                 [this](sl_element_id_t id) { return nodes_.Find(id); });
    }
  }
}

void FileSystem::SyncToDB() {
  auto& element_ctrl = storage_service_.GetElementStore();
  for (const auto& element : nodes_.DirtyElements()) {
    if (element->SyncState() == SyncFlag::UNSYNC) {
      element_ctrl.AddElement(element);
    } else if (element->SyncState() == SyncFlag::MODIFIED) {
      element_ctrl.UpdateElement(element);
    }
  }
}

void FileSystem::WriteSleeveMeta(const std::filesystem::path& meta_path) {
  nlohmann::json metadata;
  meta_path_            = meta_path;
  metadata["db_path"]   = conv::ToBytes(db_path_.wstring());
  metadata["meta_path"] = conv::ToBytes(meta_path_.wstring());
  metadata["start_id"]  = id_gen_.GetCurrentID();

  std::ofstream file(meta_path);
  if (file.is_open()) {
    file << metadata.dump(4);
    file.close();
  }
}

void FileSystem::ReadSleeveMeta(const std::filesystem::path& meta_path) {
  std::ifstream file(meta_path);
  if (file.is_open()) {
    nlohmann::json metadata;
    file >> metadata;
    db_path_   = std::filesystem::path(conv::FromBytes(metadata["db_path"]));
    meta_path_ = std::filesystem::path(conv::FromBytes(metadata["meta_path"]));
    id_gen_.SetStartID(static_cast<uint32_t>(metadata["start_id"]));
  }
}
auto FileSystem::Tree(const std::filesystem::path& path) -> std::wstring {
  return resolver_.Tree(path);
}
};  // namespace alcedo
