//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/sleeve_service.hpp"

#include <algorithm>
#include <stdexcept>
#include <unordered_set>

#include "edit/graph/pipeline_document.hpp"
#include "utils/string/convert.hpp"

namespace alcedo {
namespace {

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
  std::cout << "[LOG] SleeveServiceImpl: " << bucket << " " << ElementTypeLabel(element->type_)
            << " id=" << element->element_id_ << " name=" << conv::ToBytes(element->element_name_)
            << std::endl;
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
      if (element && element->type_ == ElementType::FILE) {
        storage_->ForgetLiveEditHistory(element->element_id_);
        storage_->ForgetLivePipeline(element->element_id_);
      }
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
  return Write<void>(
      [file_id, folder_id](FileSystem& fs) { fs.LinkFileToFolder(file_id, folder_id); });
}

auto SleeveServiceImpl::DeleteFileFromFolder(sl_element_id_t file_id,
                                             sl_element_id_t folder_id) -> SyncResult {
  return Write<void>(
      [file_id, folder_id](FileSystem& fs) { fs.UnlinkFileFromFolder(file_id, folder_id); });
}

auto SleeveServiceImpl::DeleteFilesFromFolder(std::span<const sl_element_id_t> file_ids,
                                              sl_element_id_t                  folder_id)
    -> std::pair<std::vector<std::shared_ptr<SleeveFile>>, SyncResult> {
  return Write<std::vector<std::shared_ptr<SleeveFile>>>(
      [file_ids, folder_id](FileSystem& fs) -> std::vector<std::shared_ptr<SleeveFile>> {
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
  return Write<void>([file_id](FileSystem& fs) { fs.DeleteFileEverywhere(file_id); });
}

auto SleeveServiceImpl::DeleteFilesEverywhere(std::span<const sl_element_id_t> file_ids)
    -> std::pair<std::vector<std::shared_ptr<SleeveFile>>, SyncResult> {
  return Write<std::vector<std::shared_ptr<SleeveFile>>>(
      [file_ids](FileSystem& fs) -> std::vector<std::shared_ptr<SleeveFile>> {
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
  return Write<void>([target_path](FileSystem& fs) { fs.Delete(target_path); });
}

auto SleeveServiceImpl::DeleteElement(sl_element_id_t target_id) -> SyncResult {
  return Write<void>([target_id](FileSystem& fs) { fs.Delete(target_id); });
}
};  // namespace alcedo
