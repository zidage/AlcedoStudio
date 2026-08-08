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
#include <stdexcept>
#include <unordered_set>

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
      storage_handler_(storage_service_.GetElementStore(), storage_),
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
  storage_[0] = root;
  root_       = std::static_pointer_cast<SleeveFolder>(root);
  if (root_->sync_flag_ == SyncFlag::UNSYNC) {
    root_->MarkChildrenLoaded();
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
  auto       dest_folder                  = std::static_pointer_cast<SleeveFolder>(dest_element);
  const bool create_album_file_membership = type == ElementType::FILE && !IsRootPath(dest);
  if (create_album_file_membership) {
    storage_handler_.EnsureChildrenLoaded(root_);
  }
  while (dest_folder->Contains(filename) ||
         (create_album_file_membership && root_->Contains(filename))) {
    filename = filename + L"@";
  }
  auto new_id      = id_gen_.GenerateID();
  auto new_element = SleeveElementFactory::CreateElement(type, new_id, filename);
  if (new_element->type_ == ElementType::FOLDER) {
    std::static_pointer_cast<SleeveFolder>(new_element)->MarkChildrenLoaded();
  }
  storage_[new_id] = new_element;
  if (create_album_file_membership) {
    root_->AddElementToMap(new_element);
    dest_folder->AddElementToMap(new_element, true, false);
  } else {
    dest_folder->AddElementToMap(new_element);
  }

  return new_element;
}

auto FileSystem::CreateFileInLibrary(file_name_t name) -> std::shared_ptr<SleeveFile> {
  auto element = Create(L"", std::move(name), ElementType::FILE);
  return std::static_pointer_cast<SleeveFile>(element);
}

void FileSystem::LinkFileToFolder(sl_element_id_t file_id, sl_element_id_t folder_id) {
  auto file = Get(file_id);
  if (!file || file->type_ != ElementType::FILE || file->sync_flag_ == SyncFlag::DELETED) {
    throw std::runtime_error("Filesystem: Specified file id is not a live file");
  }

  auto folder_element = Get(folder_id);
  if (!folder_element || folder_element->type_ != ElementType::FOLDER ||
      folder_element->sync_flag_ == SyncFlag::DELETED) {
    throw std::runtime_error("Filesystem: Specified folder id is not a live folder");
  }

  auto folder = std::static_pointer_cast<SleeveFolder>(folder_element);
  storage_handler_.EnsureChildrenLoaded(folder);
  if (folder->ContainsElementId(file_id)) {
    return;
  }
  if (auto existing = folder->GetElementIdByName(file->element_name_);
      existing.has_value() && existing.value() != file_id) {
    throw std::runtime_error("Filesystem: Folder already contains a different element with name");
  }

  folder->AddElementToMap(file, true, false);
}

void FileSystem::UnlinkFileFromFolder(sl_element_id_t file_id, sl_element_id_t folder_id) {
  if (folder_id == 0) {
    throw std::runtime_error("Filesystem: Root file removal must use DeleteFileEverywhere");
  }

  auto file = Get(file_id);
  if (!file || file->type_ != ElementType::FILE || file->sync_flag_ == SyncFlag::DELETED) {
    throw std::runtime_error("Filesystem: Specified file id is not a live file");
  }

  auto folder_element = Get(folder_id);
  if (!folder_element || folder_element->type_ != ElementType::FOLDER ||
      folder_element->sync_flag_ == SyncFlag::DELETED) {
    throw std::runtime_error("Filesystem: Specified folder id is not a live folder");
  }

  auto folder = std::static_pointer_cast<SleeveFolder>(folder_element);
  storage_handler_.EnsureChildrenLoaded(folder);
  if (!folder->RemoveElementById(file_id)) {
    throw std::runtime_error("Filesystem: File is not a member of the folder");
  }
}

auto FileSystem::UnlinkFilesFromFolder(std::span<const sl_element_id_t> file_ids,
                                       sl_element_id_t                  folder_id)
    -> std::vector<sl_element_id_t> {
  if (folder_id == 0) {
    throw std::runtime_error("Filesystem: Root file removal must use DeleteFilesEverywhere");
  }

  auto folder_element = Get(folder_id);
  if (!folder_element || folder_element->type_ != ElementType::FOLDER ||
      folder_element->sync_flag_ == SyncFlag::DELETED) {
    throw std::runtime_error("Filesystem: Specified folder id is not a live folder");
  }

  auto folder = std::static_pointer_cast<SleeveFolder>(folder_element);
  storage_handler_.EnsureChildrenLoaded(folder);

  std::vector<sl_element_id_t> removed_ids;
  removed_ids.reserve(file_ids.size());
  std::unordered_set<sl_element_id_t> seen;
  seen.reserve(file_ids.size() * 2 + 1);

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
    if (!file || file->type_ != ElementType::FILE || file->sync_flag_ == SyncFlag::DELETED) {
      continue;
    }
    if (folder->RemoveElementById(file_id)) {
      removed_ids.push_back(file_id);
    }
  }
  return removed_ids;
}

auto FileSystem::DuplicateFileToFolder(sl_element_id_t file_id, sl_element_id_t folder_id)
    -> std::shared_ptr<SleeveFile> {
  auto source = Get(file_id);
  if (!source || source->type_ != ElementType::FILE || source->sync_flag_ == SyncFlag::DELETED) {
    throw std::runtime_error("Filesystem: Specified file id is not a live file");
  }

  auto folder_element = Get(folder_id);
  if (!folder_element || folder_element->type_ != ElementType::FOLDER ||
      folder_element->sync_flag_ == SyncFlag::DELETED) {
    throw std::runtime_error("Filesystem: Specified folder id is not a live folder");
  }

  auto folder = std::static_pointer_cast<SleeveFolder>(folder_element);
  storage_handler_.EnsureChildrenLoaded(folder);
  if (folder_id != 0) {
    storage_handler_.EnsureChildrenLoaded(root_);
  }

  auto source_file = std::static_pointer_cast<SleeveFile>(source);
  auto source_history = storage_service_.GetLiveEditHistory(source_file->element_id_);
  if (!source_history) {
    try {
      source_history =
          storage_service_.GetElementStore().GetEditHistoryByFileId(source_file->element_id_);
    } catch (...) {
    }
  }
  if (source_history) {
    source_file->SetEditHistory(source_history);
  }

  auto duplicate_name = source->element_name_;
  while (folder->Contains(duplicate_name) || (folder_id != 0 && root_->Contains(duplicate_name))) {
    duplicate_name += L"@";
  }

  auto duplicated       = std::static_pointer_cast<SleeveFile>(source_file->Copy(id_gen_.GenerateID()));
  duplicated->element_name_ = duplicate_name;
  duplicated->image_id_ = source_file->image_id_;
  storage_[duplicated->element_id_] = duplicated;
  if (folder_id == 0) {
    folder->AddElementToMap(duplicated);
  } else {
    root_->AddElementToMap(duplicated);
    folder->AddElementToMap(duplicated, true, false);
  }
  return duplicated;
}

void FileSystem::DeleteFileEverywhere(sl_element_id_t file_id) {
  if (file_id == 0) {
    throw std::runtime_error("Filesystem: root cannot be deleted");
  }

  auto file = Get(file_id);
  if (!file || file->type_ != ElementType::FILE || file->sync_flag_ == SyncFlag::DELETED) {
    throw std::runtime_error("Filesystem: Specified file id is not a live file");
  }

  for (auto& [_, element] : storage_) {
    if (!element || element->type_ != ElementType::FOLDER ||
        element->sync_flag_ == SyncFlag::DELETED) {
      continue;
    }
    auto folder = std::static_pointer_cast<SleeveFolder>(element);
    if (folder->ChildrenLoaded()) {
      folder->RemoveElementById(file_id);
    }
  }

  file->SetSyncFlag(SyncFlag::DELETED);
}

auto FileSystem::DeleteFilesEverywhere(std::span<const sl_element_id_t> file_ids)
    -> std::vector<sl_element_id_t> {
  std::vector<sl_element_id_t> deleted_ids;
  deleted_ids.reserve(file_ids.size());
  std::unordered_set<sl_element_id_t> seen;
  seen.reserve(file_ids.size() * 2 + 1);

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
    if (!file || file->type_ != ElementType::FILE || file->sync_flag_ == SyncFlag::DELETED) {
      continue;
    }
    for (auto& [_, element] : storage_) {
      if (!element || element->type_ != ElementType::FOLDER ||
          element->sync_flag_ == SyncFlag::DELETED) {
        continue;
      }
      auto folder = std::static_pointer_cast<SleeveFolder>(element);
      if (folder->ChildrenLoaded()) {
        folder->RemoveElementById(file_id);
      }
    }
    file->SetSyncFlag(SyncFlag::DELETED);
    deleted_ids.push_back(file_id);
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

auto FileSystem::ListFolderContent(const std::filesystem::path& folder_path, bool write)
    -> std::vector<sl_element_id_t> {
  std::shared_ptr<SleeveElement> folder_element;
  if (write) {
    folder_element = resolver_.ResolveForWrite(folder_path);
  } else {
    folder_element = resolver_.Resolve(folder_path);
  }

  if (!folder_element || folder_element->type_ != ElementType::FOLDER) {
    throw std::runtime_error("Filesystem: Specified path is not a folder");
  }

  auto folder = std::static_pointer_cast<SleeveFolder>(folder_element);
  storage_handler_.EnsureChildrenLoaded(folder);
  const auto& elements = folder->ListElements();
  return std::vector<sl_element_id_t>(elements.begin(), elements.end());
}

auto FileSystem::ListFolderContent(sl_element_id_t folder_id) -> std::vector<sl_element_id_t> {
  auto folder_element = Get(folder_id);
  if (!folder_element || folder_element->type_ != ElementType::FOLDER) {
    throw std::runtime_error("Filesystem: Specified id is not a folder");
  }

  auto folder = std::static_pointer_cast<SleeveFolder>(folder_element);
  storage_handler_.EnsureChildrenLoaded(folder);
  const auto& elements = folder->ListElements();
  return std::vector<sl_element_id_t>(elements.begin(), elements.end());
}

void FileSystem::Delete(std::filesystem::path target) {
  if (!target.has_parent_path()) {
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
  if (delete_node->type_ == ElementType::FILE) {
    if (IsRootPath(parent)) {
      DeleteFileEverywhere(delete_node->element_id_);
    } else {
      UnlinkFileFromFolder(delete_node->element_id_, parent_node->element_id_);
    }
    resolver_.Invalidate(target);
    return;
  }

  // Now re-acquire parent_node using write method for folder-tree mutation.
  auto parent_write_node =
      std::static_pointer_cast<SleeveFolder>(resolver_.ResolveForWrite(parent));
  delete_node->DecrementRefCount();

  if (delete_node->ref_count_ <= 0) {
    // Mark the node as deleted in storage handler
    // Album membership does not own files. Removing a folder only decrements nested folder
    // identities that participate in folder-tree CoW; file children remain live library files.
    if (delete_node->type_ == ElementType::FOLDER) {
      auto delete_folder = std::static_pointer_cast<SleeveFolder>(delete_node);
      storage_handler_.EnsureChildrenLoaded(delete_folder);
      auto& children = delete_folder->ListElements();
      for (auto& child_id : children) {
        auto child = storage_handler_.GetElement(child_id);
        if (!child || child->sync_flag_ == SyncFlag::DELETED || child->type_ != ElementType::FOLDER) {
          continue;
        }
        child->DecrementRefCount();
        if (child->ref_count_ <= 0) {
          // Recursively decrement
          std::filesystem::path child_path =
              target / child->element_name_;  // Construct child's path for resolver
          Delete(child_path);
        }
      }
    }
  }

  parent_write_node->RemoveNameFromMap(delete_node_name.wstring());
  resolver_.Invalidate(target);
}

void FileSystem::Delete(sl_element_id_t target_id) {
  if (target_id == 0) {
    throw std::runtime_error("Filesystem: root cannot be deleted");
  }

  auto delete_node = Get(target_id);
  if (!delete_node || delete_node->sync_flag_ == SyncFlag::DELETED) {
    throw std::runtime_error("Filesystem: Deleting node does not exist");
  }
  if (delete_node->type_ == ElementType::FILE) {
    DeleteFileEverywhere(target_id);
    return;
  }

  const auto decrement_folder_children = [this](const std::shared_ptr<SleeveFolder>& folder,
                                                const auto&                          self) -> void {
    storage_handler_.EnsureChildrenLoaded(folder);
    const auto children = folder->ListElements();
    for (const auto child_id : children) {
      auto child = storage_handler_.GetElement(child_id);
      if (!child || child->sync_flag_ == SyncFlag::DELETED || child->type_ != ElementType::FOLDER) {
        continue;
      }
      child->DecrementRefCount();
      if (child->ref_count_ <= 0) {
        self(std::static_pointer_cast<SleeveFolder>(child), self);
      }
    }
  };

  const auto remove_from_folder = [this, target_id, &delete_node, &decrement_folder_children](
                                      const std::shared_ptr<SleeveFolder>& folder,
                                      const auto&                          self) -> bool {
    storage_handler_.EnsureChildrenLoaded(folder);
    const auto children = folder->ListElements();
    for (const auto child_id : children) {
      auto child = storage_handler_.GetElement(child_id);
      if (!child || child->sync_flag_ == SyncFlag::DELETED) {
        continue;
      }
      if (child_id == target_id) {
        delete_node->DecrementRefCount();
        if (delete_node->ref_count_ <= 0 && delete_node->type_ == ElementType::FOLDER) {
          const auto delete_folder = std::static_pointer_cast<SleeveFolder>(delete_node);
          decrement_folder_children(delete_folder, decrement_folder_children);
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
  if (from_node->type_ == ElementType::FILE) {
    auto dest_node = std::static_pointer_cast<SleeveFolder>(resolver_.Resolve(dest));
    LinkFileToFolder(from_node->element_id_, dest_node->element_id_);
    return;
  }

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

auto FileSystem::GetModifiedElements() -> std::vector<std::shared_ptr<SleeveElement>> {
  std::vector<std::shared_ptr<SleeveElement>> modified_elements;
  for (auto& pair : storage_) {
    auto element = pair.second;
    if (element->sync_flag_ == SyncFlag::MODIFIED) {
      modified_elements.push_back(element);
    }
  }
  return modified_elements;
}

auto FileSystem::GetUnsyncedElements() -> std::vector<std::shared_ptr<SleeveElement>> {
  std::vector<std::shared_ptr<SleeveElement>> unsynced_elements;
  for (auto& pair : storage_) {
    auto element = pair.second;
    if (element->sync_flag_ == SyncFlag::UNSYNC) {
      unsynced_elements.push_back(element);
    }
  }
  return unsynced_elements;
}

auto FileSystem::GetDeletedElements() -> std::vector<std::shared_ptr<SleeveElement>> {
  std::vector<std::shared_ptr<SleeveElement>> deleted_elements;
  for (auto& pair : storage_) {
    auto element = pair.second;
    if (element->sync_flag_ == SyncFlag::DELETED) {
      deleted_elements.push_back(element);
    }
  }
  return deleted_elements;
}

void FileSystem::SyncToDB() {
  auto& element_ctrl = storage_service_.GetElementStore();
  for (auto& pair : storage_) {
    auto element = pair.second;
    if (element->sync_flag_ == SyncFlag::UNSYNC) {
      element_ctrl.AddElement(element);
    } else if (element->sync_flag_ == SyncFlag::MODIFIED) {
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
