//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "sleeve/storage.hpp"

#include <exception>
#include <mutex>

namespace alcedo {
NodeStorageHandler::NodeStorageHandler(
    ElementStore&                                                   element_store,
    std::unordered_map<sl_element_id_t, std::shared_ptr<SleeveElement>>& storage)
    : element_store_(element_store), storage_(storage) {}

void NodeStorageHandler::AddToStorage(std::shared_ptr<SleeveElement> new_element) {
  storage_[new_element->element_id_] = new_element;
}

auto NodeStorageHandler::GetElement(uint32_t id) -> std::shared_ptr<SleeveElement> {
  if (storage_.contains(id)) {
    return storage_.at(id);
  }
  // If the element is not presented in the memory, get it from the db.
  // Then loaded pointer into the storage
  auto result                   = element_store_.GetElementById(id);
  storage_[result->element_id_] = result;
  return result;
}

void NodeStorageHandler::EnsureChildrenLoaded(std::shared_ptr<SleeveFolder> folder) {
  if (folder->ChildrenLoaded()) {
    return;
  }

  try {
    auto folder_content = element_store_.GetFolderContent(folder->element_id_);
    for (auto& content_id : folder_content) {
      auto content = GetElement(content_id);
      if (!content || content->sync_flag_ == SyncFlag::DELETED) {
        continue;
      }
      // DB-backed children already carry persisted ref counts. Reloading the in-memory
      // folder map must not add an extra parent reference or the next write will trigger
      // a bogus copy-on-write clone.
      folder->AddElementToMap(content, false, false);
    }
    folder->MarkChildrenLoaded();
  } catch (std::exception& e) {
    // TODO: LOG
  }
}

void NodeStorageHandler::GarbageCollect() {
  std::vector<sl_element_id_t> to_delete;
  for (auto& pair : storage_) {
    auto element = pair.second;
    if (element->sync_flag_ == SyncFlag::DELETED) {
      to_delete.push_back(element->element_id_);
    }
  }
  for (auto id : to_delete) {
    storage_.erase(id);
  }
}

Storage::Storage(std::filesystem::path db_path)
    : database_(db_path),
      element_store_(database_.GetConnectionGuard()),
      image_store_(database_.GetConnectionGuard()),
      semantic_store_(database_),
      ai_store_(database_) {}

auto Storage::GetElementStore() -> ElementStore& { return element_store_; }

auto Storage::GetImageStore() -> ImageStore& { return image_store_; }

auto Storage::GetSemanticStore() -> SemanticStore& {
  return semantic_store_;
}

auto Storage::GetAiStore() -> AiStore& {
  return ai_store_;
}

auto Storage::GetDatabase() -> Database& { return database_; }

void Storage::RememberLiveEditHistory(const sl_element_id_t               file_id,
                                             const std::shared_ptr<EditHistory>& history) {
  std::lock_guard<std::mutex> lock(live_state_lock_);
  if (!history) {
    live_histories_.erase(file_id);
    return;
  }
  live_histories_[file_id] = history;
}

auto Storage::GetLiveEditHistory(const sl_element_id_t file_id)
    -> std::shared_ptr<EditHistory> {
  std::lock_guard<std::mutex> lock(live_state_lock_);
  const auto                  it = live_histories_.find(file_id);
  if (it == live_histories_.end()) {
    return nullptr;
  }

  auto history = it->second.lock();
  if (!history) {
    live_histories_.erase(it);
  }
  return history;
}

void Storage::ForgetLiveEditHistory(const sl_element_id_t file_id) {
  std::lock_guard<std::mutex> lock(live_state_lock_);
  live_histories_.erase(file_id);
}

void Storage::RememberLivePipeline(const sl_element_id_t                       file_id,
                                          const std::shared_ptr<CPUPipelineExecutor>& pipeline) {
  std::lock_guard<std::mutex> lock(live_state_lock_);
  if (!pipeline) {
    live_pipelines_.erase(file_id);
    return;
  }
  live_pipelines_[file_id] = pipeline;
}

auto Storage::GetLivePipeline(const sl_element_id_t file_id)
    -> std::shared_ptr<CPUPipelineExecutor> {
  std::lock_guard<std::mutex> lock(live_state_lock_);
  const auto                  it = live_pipelines_.find(file_id);
  if (it == live_pipelines_.end()) {
    return nullptr;
  }
  return it->second;
}

void Storage::ForgetLivePipeline(const sl_element_id_t file_id) {
  std::lock_guard<std::mutex> lock(live_state_lock_);
  live_pipelines_.erase(file_id);
}
};  // namespace alcedo
