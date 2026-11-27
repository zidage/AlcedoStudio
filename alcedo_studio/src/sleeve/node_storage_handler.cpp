//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "sleeve/node_storage_handler.hpp"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <memory>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "utils/logging/log.hpp"
#include "utils/string/convert.hpp"

namespace alcedo {
namespace {
/// The entry of @p folder at @p path: copies of its fields (#1178).
auto MakeFolderTreeEntry(std::filesystem::path path, const SleeveFolder& folder)
    -> FolderTreeEntry {
  return {std::move(path), folder.element_id_, folder.element_name_, folder.mount_id_};
}
}  // namespace

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

auto NodeStorageHandler::IsDeletedInCache(sl_element_id_t id) const -> bool {
  const auto cached = nodes_.Find(id);
  return cached && !cached->IsLive();
}

auto NodeStorageHandler::FileIds(const SleeveFolder& folder) const -> std::vector<sl_element_id_t> {
  std::vector<sl_element_id_t> file_ids;
  if (folder.SyncState() != SyncFlag::UNSYNC) {
    file_ids = folder_entry_store_.ReadFolderFiles(folder.element_id_);
  }
  const auto& unlinked = folder.ContentRemovedSinceSync();
  std::erase_if(file_ids,
                [&](sl_element_id_t id) { return unlinked.contains(id) || IsDeletedInCache(id); });
  // A pending add is a file link unless the id names a child folder.
  for (const auto id : folder.ContentAddedSinceSync()) {
    if (!folder.HoldsChildFolder(id) && !IsDeletedInCache(id)) {
      file_ids.push_back(id);
    }
  }
  std::sort(file_ids.begin(), file_ids.end());
  file_ids.erase(std::unique(file_ids.begin(), file_ids.end()), file_ids.end());
  return file_ids;
}

auto NodeStorageHandler::HeldFiles(const SleeveFolder&              folder,
                                   std::span<const sl_element_id_t> file_ids) const
    -> std::unordered_set<sl_element_id_t> {
  std::unordered_set<sl_element_id_t> held;
  std::vector<sl_element_id_t>        unknown;
  for (const auto id : file_ids) {
    if (IsDeletedInCache(id) || folder.HoldsChildFolder(id)) {
      continue;
    }
    if (const auto pending = folder.PendingMembership(id); pending.has_value()) {
      if (*pending) {
        held.insert(id);
      }
      continue;
    }
    unknown.push_back(id);
  }
  if (!unknown.empty() && folder.SyncState() != SyncFlag::UNSYNC) {
    for (const auto id : folder_entry_store_.ReadHeldFiles(folder.element_id_, unknown)) {
      held.insert(id);
    }
  }
  return held;
}

auto NodeStorageHandler::LiveFileIds(const SleeveFolder&              root,
                                     std::span<const sl_element_id_t> ids) const
    -> std::vector<sl_element_id_t> {
  std::vector<sl_element_id_t> stored_ids;
  for (const auto id : ids) {
    if (!nodes_.Contains(id)) {
      stored_ids.push_back(id);
    }
  }
  // Each live file has its link in the library root (D2).
  const auto                          live_stored = HeldFiles(root, stored_ids);
  std::vector<sl_element_id_t>        live_ids;
  std::unordered_set<sl_element_id_t> seen;
  for (const auto id : ids) {
    const auto cached = nodes_.Find(id);
    const bool live =
        cached ? cached->type_ == ElementType::FILE && cached->IsLive() : live_stored.contains(id);
    if (live && seen.insert(id).second) {
      live_ids.push_back(id);
    }
  }
  return live_ids;
}

auto NodeStorageHandler::MountFileLinks(mount_id_t                                     mount_id,
                                        std::span<const std::shared_ptr<SleeveFolder>> folders)
    -> std::vector<FileLink> {
  const auto is_mount_file = [mount_id](const SleeveElement& element) {
    return element.type_ == ElementType::FILE && element.IsLive() && element.mount_id_ == mount_id;
  };
  std::unordered_map<sl_element_id_t, const SleeveFolder*> folder_at;
  bool                                                     any_stored = false;
  for (const auto& folder : folders) {
    folder_at.emplace(folder->element_id_, folder.get());
    any_stored = any_stored || folder->SyncState() != SyncFlag::UNSYNC;
  }

  std::vector<FileLink> links;
  if (any_stored) {
    for (const auto& row : folder_entry_store_.ReadMountFileFolders(mount_id)) {
      const auto folder = folder_at.find(row.folder_id);
      // A pending change of the folder decides the file below.
      if (folder == folder_at.end() || folder->second->PendingMembership(row.file_id).has_value()) {
        continue;
      }
      const auto cached = nodes_.Find(row.file_id);
      if (cached ? is_mount_file(*cached) : row.file_mount_id == mount_id) {
        links.push_back(FileLink{row.file_id, row.folder_id});
      }
    }
  }
  for (const auto& folder : folders) {
    for (const auto id : folder->ContentAddedSinceSync()) {
      if (folder->HoldsChildFolder(id) || IsDeletedInCache(id)) {
        continue;
      }
      if (is_mount_file(*GetElement(id))) {
        links.push_back(FileLink{id, folder->element_id_});
      }
    }
  }
  std::sort(links.begin(), links.end());
  return links;
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

auto NodeStorageHandler::ChildFolderEntries(const SleeveFolder&          folder,
                                            const std::filesystem::path& folder_path) const
    -> std::vector<FolderTreeEntry> {
  const auto                   children = ChildFolders(folder);
  std::vector<FolderTreeEntry> entries;
  entries.reserve(children.size());
  for (const auto& child : children) {
    entries.push_back(MakeFolderTreeEntry(folder_path / child->element_name_, *child));
  }
  return entries;
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
    entries.push_back(MakeFolderTreeEntry(path, *child));
    on_path.insert(child->element_id_);
    auto       grandchildren = ChildFolders(*child);
    const auto id            = child->element_id_;
    stack.push_back({std::move(grandchildren), 0, std::move(path), id});
  }
  return entries;
}

}  // namespace alcedo
