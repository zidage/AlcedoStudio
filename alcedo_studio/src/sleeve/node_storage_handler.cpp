//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "sleeve/node_storage_handler.hpp"

#include <cstdint>
#include <exception>
#include <filesystem>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "utils/logging/log.hpp"
#include "utils/string/convert.hpp"

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

void NodeStorageHandler::LoadFolderTree() {
  FolderTreeRows tree;
  try {
    tree = folder_entry_store_.ReadFolderTree();
  } catch (const std::exception& e) {
    ALCEDO_LOG_ERROR(Library) << "NodeStorageHandler: the folder tree did not load: " << e.what();
    throw;
  }

  // The stored folders in id order; a folder that the cache holds (the root) keeps its object.
  std::vector<sl_element_id_t>                                       folder_ids;
  std::unordered_map<sl_element_id_t, std::shared_ptr<SleeveFolder>> folders;
  folder_ids.reserve(tree.folders_.size());
  folders.reserve(tree.folders_.size());
  for (auto& stored : tree.folders_) {
    const auto id = stored->element_id_;
    if (const auto cached = nodes_.Find(id)) {
      if (cached->type_ != ElementType::FOLDER) {
        continue;
      }
      folders.emplace(id, std::static_pointer_cast<SleeveFolder>(cached));
    } else {
      nodes_.Insert(stored);
      folders.emplace(id, std::move(stored));
    }
    folder_ids.push_back(id);
  }

  std::unordered_map<sl_element_id_t, std::vector<sl_element_id_t>> children_by_parent;
  for (const auto& [parent_id, child_id] : tree.edges_) {
    children_by_parent[parent_id].push_back(child_id);
  }
  const std::vector<sl_element_id_t> no_children;
  const auto children_of = [&](sl_element_id_t id) -> const std::vector<sl_element_id_t>& {
    const auto it = children_by_parent.find(id);
    return it == children_by_parent.end() ? no_children : it->second;
  };

  // A folder is on the current walk path from its push to its pop, and done after it.
  enum class WalkState : std::uint8_t { kOnPath, kDone };
  struct Frame {
    SleeveFolder*                       folder;
    const std::vector<sl_element_id_t>* children;
    std::size_t                         next;
  };
  std::unordered_map<sl_element_id_t, WalkState> states;
  states.reserve(folders.size());
  std::vector<Frame> stack;

  const auto         walk_from = [&](SleeveFolder& start) {
    states[start.element_id_] = WalkState::kOnPath;
    stack.push_back({&start, &children_of(start.element_id_), 0});
    while (!stack.empty()) {
      auto& top = stack.back();
      if (top.next == top.children->size()) {
        states[top.folder->element_id_] = WalkState::kDone;
        stack.pop_back();
        continue;
      }
      SleeveFolder* parent   = top.folder;
      const auto    child_id = (*top.children)[top.next++];
      const auto    child_it = folders.find(child_id);
      if (child_it == folders.end()) {
        continue;
      }
      const auto& child      = child_it->second;
      const auto  state_it   = states.find(child_id);
      const bool  discovered = state_it != states.end();
      if (discovered && state_it->second == WalkState::kOnPath) {
        ALCEDO_LOG_WARN(Library) << "NodeStorageHandler: the folder link " << parent->element_id_
                                 << " -> " << child_id
                                 << " closes a cycle of folders; the link is skipped";
        continue;
      }
      if (const auto existing = parent->GetElementIdByName(child->element_name_);
          existing.has_value() && existing.value() != child_id) {
        ALCEDO_LOG_WARN(Library) << "NodeStorageHandler: folder " << parent->element_id_
                                 << " holds the name of folder " << child_id << " for folder "
                                 << existing.value() << "; the link is skipped";
        continue;
      }
      // A stored link: no reference count change, no pending content change.
      parent->AddElementToMap(child, false, false);
      if (!discovered) {
        states.emplace(child_id, WalkState::kOnPath);
        stack.push_back({child.get(), &children_of(child_id), 0});
      }
    }
  };

  if (const auto root = folders.find(0); root != folders.end()) {
    walk_from(*root->second);
  }
  // A folder that the root does not reach still gets its child folders.
  for (const auto id : folder_ids) {
    if (!states.contains(id)) {
      walk_from(*folders.at(id));
    }
  }
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
      // The child folders came with LoadFolderTree, and the folder entries are the current
      // ones: a stored link to a folder or to a removed file is not a child any more.
      if (!content || !content->IsLive() || content->type_ != ElementType::FILE ||
          folder->ContentRemovedSinceSync().contains(content_id)) {
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

auto NodeStorageHandler::ChildFolders(const SleeveFolder& folder) const
    -> std::vector<std::shared_ptr<SleeveFolder>> {
  std::vector<std::shared_ptr<SleeveFolder>> folders;
  for (const auto child_id : folder.ListElements()) {
    const auto child = nodes_.Find(child_id);
    if (child && child->type_ == ElementType::FOLDER && child->IsLive()) {
      folders.push_back(std::static_pointer_cast<SleeveFolder>(child));
    }
  }
  return folders;
}

auto NodeStorageHandler::FolderTree(const SleeveFolder& root) const
    -> std::vector<FolderTreeEntry> {
  struct Frame {
    std::vector<std::shared_ptr<SleeveFolder>> children;
    std::size_t                                next;
    std::filesystem::path                      path;
    sl_element_id_t                            id;
  };
  std::vector<FolderTreeEntry>        entries;
  std::unordered_set<sl_element_id_t> on_path{root.element_id_};
  std::vector<Frame>                  stack;
  stack.push_back({ChildFolders(root), 0, std::filesystem::path{L"/"}, root.element_id_});
  while (!stack.empty()) {
    auto& top = stack.back();
    if (top.next == top.children.size()) {
      on_path.erase(top.id);
      stack.pop_back();
      continue;
    }
    auto child = top.children[top.next++];
    if (on_path.contains(child->element_id_)) {
      ALCEDO_LOG_WARN(Library) << "NodeStorageHandler: folder " << child->element_id_
                               << " is its own ancestor at "
                               << conv::ToBytes(top.path.generic_wstring())
                               << "; the folder tree skips it there";
      continue;
    }
    auto path = top.path / child->element_name_;
    entries.push_back({path, child});
    on_path.insert(child->element_id_);
    auto       grandchildren = ChildFolders(*child);
    const auto id            = child->element_id_;
    stack.push_back({std::move(grandchildren), 0, std::move(path), id});
  }
  return entries;
}

}  // namespace alcedo
