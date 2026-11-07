//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "sleeve/path_resolver.hpp"

#include <xxhash.h>

#include <algorithm>
#include <exception>
#include <filesystem>
#include <functional>
#include <memory>
#include <sstream>
#include <stack>
#include <stdexcept>
#include <string>
#include <vector>

#include "sleeve/sleeve_element/sleeve_element.hpp"
#include "sleeve/sleeve_element/sleeve_folder.hpp"
#include "type/type.hpp"
#include "utils/id/id_generator.hpp"

namespace alcedo {
PathResolver::PathResolver(NodeStorageHandler& lazy_handler, IncrID::IDGenerator<uint32_t>& id_gen)
    : storage_handler_(lazy_handler), id_gen_(id_gen) {}

void PathResolver::SetRoot(std::shared_ptr<SleeveFolder> root) { root_ = root; }

auto PathResolver::Normalize(const std::filesystem::path raw_path) -> std::wstring {
  return raw_path.lexically_normal().wstring();
}

auto PathResolver::IsSubpath(const std::filesystem::path& base, const std::filesystem::path& target)
    -> bool {
  auto mismatch = std::mismatch(base.begin(), base.end(), target.begin());
  return mismatch.first == base.end();
}

auto PathResolver::Contains(const std::filesystem::path& path) -> bool {
  try {
    Resolve(path);
  } catch (...) {
    return false;
  }
  return true;
}

auto PathResolver::Contains(const std::filesystem::path& path, ElementType type) -> bool {
  try {
    auto target = Resolve(path);
    if (target->type_ != type) {
      return false;
    }
  } catch (...) {
    return false;
  }
  return true;
}

auto PathResolver::Resolve(const std::filesystem::path& path) -> std::shared_ptr<SleeveElement> {
  std::shared_ptr<SleeveElement> current    = root_;
  auto                           visit_path = path.relative_path();

  auto                           cached_id  = directory_cache_.AccessElement(path.wstring());
  if (cached_id.has_value()) {
    auto cached = storage_handler_.GetElement(cached_id.value());
    if (!cached || cached->sync_flag_ == SyncFlag::DELETED) {
      directory_cache_.RemoveRecord(path.wstring());
      throw std::runtime_error("Path Resolver: Illegal path. Target does not exist");
    }
    return cached;
  }

  for (const auto& part : visit_path) {
    if (current->type_ == ElementType::FILE) {
      // TODO: add customized exception class
      throw std::runtime_error("Path Resolver: Illegal path.");
    }

    std::shared_ptr<SleeveFolder> folder = std::static_pointer_cast<SleeveFolder>(current);
    storage_handler_.EnsureChildrenLoaded(folder);

    auto next_id = folder->GetElementIdByName(part.wstring());
    if (!next_id.has_value()) {
      throw std::runtime_error("Path Resolver: Illegal path. Target does not exist");
    }

    current = storage_handler_.GetElement(next_id.value());
    if (current->sync_flag_ == SyncFlag::DELETED) {
      throw std::runtime_error("Path Resolver: Illegal path. Target does not exist");
    }
  }
  directory_cache_.RecordAccess(path.wstring(), current->element_id_);

  if (current->type_ == ElementType::FOLDER) {
    storage_handler_.EnsureChildrenLoaded(std::static_pointer_cast<SleeveFolder>(current));
  }
  return current;
}

void PathResolver::Invalidate(const std::filesystem::path& path) {
  directory_cache_.RemoveRecord(path.wstring());
}

void PathResolver::InvalidateAll() { directory_cache_.Flush(); }

auto PathResolver::ResolveForWrite(const std::filesystem::path& path)
    -> std::shared_ptr<SleeveElement> {
  std::shared_ptr<SleeveElement> current        = root_;
  std::shared_ptr<SleeveElement> current_parent = nullptr;
  auto                           visit_path     = path.relative_path();

  for (const auto& part : visit_path) {
    if (current->type_ == ElementType::FILE) {
      throw std::runtime_error("Path Resolver: Illegal path.");
    }

    std::shared_ptr<SleeveFolder> folder = std::static_pointer_cast<SleeveFolder>(current);
    storage_handler_.EnsureChildrenLoaded(folder);

    if (folder->IsShared()) {
      folder = std::static_pointer_cast<SleeveFolder>(
          CoWHandler(current, std::static_pointer_cast<SleeveFolder>(current_parent)));
    }
    auto next_id = folder->GetElementIdByName(part.wstring());
    if (!next_id.has_value()) {
      throw std::runtime_error("Path Resolver: Illegal path. Target does not exist");
    }
    current_parent = folder;
    current        = storage_handler_.GetElement(next_id.value());
  }
  if (current->type_ == ElementType::FOLDER && current->IsShared()) {
    current = CoWHandler(current, std::static_pointer_cast<SleeveFolder>(current_parent));
  }
  if (current->sync_flag_ == SyncFlag::SYNCED) {
    current->SetSyncFlag(SyncFlag::MODIFIED);
  }
  // directory_cache_.RecordAccess(path.wstring(), current->element_id_);
  return current;
}

auto PathResolver::CoWHandler(const std::shared_ptr<SleeveElement> to_copy,
                              const std::shared_ptr<SleeveFolder>  parent_folder)
    -> std::shared_ptr<SleeveElement> {
  auto old_id = to_copy->element_id_;
  to_copy->DecrementRefCount();
  auto copied = to_copy->Copy(id_gen_.GenerateID());
  // Increment every child's ref count
  if (copied->type_ == ElementType::FOLDER) {
    auto copied_folder = std::static_pointer_cast<SleeveFolder>(copied);
    storage_handler_.EnsureChildrenLoaded(copied_folder);
    auto& contents = copied_folder->ListElements();
    for (auto& e : contents) {
      auto element = storage_handler_.GetElement(e);
      if (element && element->type_ == ElementType::FOLDER) {
        element->IncrementRefCount();
      }
    }
  }
  parent_folder->UpdateElementMap(copied->element_name_, old_id, copied->element_id_);
  copied->IncrementRefCount();
  storage_handler_.AddToStorage(copied);
  if (to_copy->sync_flag_ == SyncFlag::SYNCED) {
    to_copy->SetSyncFlag(SyncFlag::MODIFIED);
  }
  return copied;
}

auto PathResolver::Tree(const std::filesystem::path& path) -> std::wstring {
  struct TreeNode {
    sl_element_id_t id_;
    int             depth_;
    bool            is_last_;
  };

  std::wostringstream tree_str;
  auto                start_node = Resolve(path);
  if (start_node->type_ != ElementType::FOLDER) {
    throw std::runtime_error("Filesystem: Cannot call Tree() on a file node");
  }

  auto dfs_stack    = std::stack<TreeNode>();
  const auto push_children = [&dfs_stack](std::vector<sl_element_id_t> children, int depth) {
    std::sort(children.begin(), children.end(), std::greater<sl_element_id_t>{});
    for (auto& e : children) {
      dfs_stack.push({e, depth, false});
    }
  };

  auto visit_folder = std::static_pointer_cast<SleeveFolder>(start_node);
  storage_handler_.EnsureChildrenLoaded(visit_folder);

  auto contains = visit_folder->ListElements();
  push_children(std::vector<sl_element_id_t>(contains.begin(), contains.end()), 0);

  tree_str << visit_folder->element_name_ << L"id:" << std::to_wstring(visit_folder->element_id_)
           << L"\n";
  std::shared_ptr<SleeveElement> parent = nullptr;

  while (!dfs_stack.empty()) {
    auto next_visit = dfs_stack.top();
    dfs_stack.pop();
    auto next_visit_element = storage_handler_.GetElement(next_visit.id_);

    if (next_visit_element->type_ == ElementType::FOLDER) {
      for (int i = 0; i < next_visit.depth_; ++i) {
        tree_str << L"    ";
      }
      tree_str << L"├── " << next_visit_element->element_name_ << L" id:"
               << std::to_wstring(next_visit_element->element_id_) << L"\n";
      auto sub_folder = std::static_pointer_cast<SleeveFolder>(next_visit_element);
      storage_handler_.EnsureChildrenLoaded(sub_folder);
      contains = sub_folder->ListElements();
      push_children(std::vector<sl_element_id_t>(contains.begin(), contains.end()),
                    next_visit.depth_ + 1);
    } else {
      for (int i = 0; i < next_visit.depth_; ++i) {
        tree_str << L"    ";
      }
      tree_str << L"└── " + next_visit_element->element_name_ << L" id:"
               << std::to_wstring(next_visit_element->element_id_) << L"\n";
    }
  }
  return tree_str.str();
}
};  // namespace alcedo
