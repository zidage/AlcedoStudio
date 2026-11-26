//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "sleeve/node_storage_handler.hpp"

#include <exception>
#include <memory>
#include <unordered_map>
#include <vector>

#include "utils/logging/log.hpp"

namespace alcedo {
NodeStorageHandler::NodeStorageHandler(ElementStore&     element_store,
                                       FolderEntryStore& folder_entry_store, NodeCache& nodes)
    : element_store_(element_store), folder_entry_store_(folder_entry_store), nodes_(nodes) {}

void NodeStorageHandler::AddToStorage(std::shared_ptr<SleeveElement> new_element) {
  nodes_.Insert(std::move(new_element));
}

auto NodeStorageHandler::GetElement(sl_element_id_t id) -> std::shared_ptr<SleeveElement> {
  if (auto cached = nodes_.Find(id)) {
    return cached;
  }
  auto result = element_store_.GetElementById(id);
  nodes_.Insert(result);
  return result;
}

void NodeStorageHandler::EnsureChildrenLoaded(const std::shared_ptr<SleeveFolder>& folder) {
  if (folder->ChildrenLoaded()) {
    return;
  }

  try {
    auto folder_content = element_store_.GetFolderContent(folder->element_id_);
    // Load the children that are not in memory with one query for the folder instead of
    // one GetElementById per child. Elements already in memory keep their live state.
    std::unordered_map<sl_element_id_t, std::shared_ptr<SleeveElement>> loaded_children;
    for (auto& child : element_store_.GetFolderChildren(folder->element_id_)) {
      if (!nodes_.Contains(child->element_id_)) {
        loaded_children.emplace(child->element_id_, std::move(child));
      }
    }
    for (auto& content_id : folder_content) {
      std::shared_ptr<SleeveElement> content;
      if (auto it = loaded_children.find(content_id); it != loaded_children.end()) {
        content = it->second;
        nodes_.Insert(content);
      } else {
        content = GetElement(content_id);
      }
      if (!content || !content->IsLive()) {
        continue;
      }
      // DB-backed children already carry persisted ref counts. Reloading the in-memory
      // folder map must not add an extra parent reference or the next write will trigger
      // a bogus copy-on-write clone.
      folder->AddElementToMap(content, false, false);
    }
    folder->MarkChildrenLoaded();
  } catch (const std::exception& e) {
    ALCEDO_LOG_WARN(Library) << "NodeStorageHandler: the children of folder " << folder->element_id_
                             << " did not load: " << e.what();
  }
}

auto NodeStorageHandler::ChildFolders(const SleeveFolder& folder)
    -> std::vector<std::shared_ptr<SleeveFolder>> {
  std::vector<std::shared_ptr<SleeveFolder>> folders;
  const auto add_live_folder = [&folders](const std::shared_ptr<SleeveElement>& element) {
    if (element && element->type_ == ElementType::FOLDER && element->IsLive()) {
      folders.push_back(std::static_pointer_cast<SleeveFolder>(element));
    }
  };

  if (folder.ChildrenLoaded()) {
    for (const auto child_id : folder.ListElements()) {
      add_live_folder(GetElement(child_id));
    }
    return folders;
  }

  auto stored_folders = folder_entry_store_.ReadChildFolders(folder.element_id_);
  folders.reserve(stored_folders.size());
  for (auto& stored : stored_folders) {
    if (auto cached = nodes_.Find(stored->element_id_)) {
      add_live_folder(cached);
      continue;
    }
    nodes_.Insert(stored);
    folders.push_back(std::move(stored));
  }
  return folders;
}

}  // namespace alcedo
