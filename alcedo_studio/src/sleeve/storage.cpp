//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "sleeve/storage.hpp"

#include <exception>
#include <memory>
#include <unordered_map>

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
    // Load the children that are not in memory with one query for the folder instead of
    // one GetElementById per child. Elements already in memory keep their live state.
    std::unordered_map<sl_element_id_t, std::shared_ptr<SleeveElement>> loaded_children;
    for (auto& child : element_store_.GetFolderChildren(folder->element_id_)) {
      if (!storage_.contains(child->element_id_)) {
        loaded_children.emplace(child->element_id_, std::move(child));
      }
    }
    for (auto& content_id : folder_content) {
      std::shared_ptr<SleeveElement> content;
      if (auto it = loaded_children.find(content_id); it != loaded_children.end()) {
        content              = it->second;
        storage_[content_id] = content;
      } else {
        content = GetElement(content_id);
      }
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
      semantic_models_(database_),
      semantic_embeddings_(database_),
      semantic_labels_(database_),
      semantic_vector_search_(database_),
      ai_store_(database_) {}

auto Storage::GetElementStore() -> ElementStore& { return element_store_; }

auto Storage::GetImageStore() -> ImageStore& { return image_store_; }

auto Storage::GetSemanticModelRegistry() -> SemanticModelRegistry& { return semantic_models_; }

auto Storage::GetSemanticEmbeddingStore() -> SemanticEmbeddingStore& {
  return semantic_embeddings_;
}

auto Storage::GetSemanticLabelStore() -> SemanticLabelStore& { return semantic_labels_; }

auto Storage::GetSemanticVectorSearch() -> SemanticVectorSearch& {
  return semantic_vector_search_;
}

auto Storage::GetAiStore() -> AiStore& {
  return ai_store_;
}

auto Storage::GetDatabase() -> Database& { return database_; }
};  // namespace alcedo
