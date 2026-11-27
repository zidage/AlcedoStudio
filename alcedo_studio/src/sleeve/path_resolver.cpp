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
    // The sync removes a deleted element from the node cache and from the store, so the element
    // of an old record may not load. Such a record goes, and the walk from the root gives the
    // folder that holds the path now.
    std::shared_ptr<SleeveElement> cached;
    try {
      cached = storage_handler_.GetElement(cached_id.value());
    } catch (const std::exception&) {
      cached.reset();
    }
    if (cached && cached->IsLive() && cached->type_ == ElementType::FOLDER) {
      return cached;
    }
    directory_cache_.RemoveRecord(path.wstring());
  }

  for (const auto& part : visit_path) {
    // The child folders are resident and a file has no path.
    const auto folder  = std::static_pointer_cast<SleeveFolder>(current);
    const auto next_id = folder->GetElementIdByName(part.wstring());
    if (!next_id.has_value()) {
      throw std::runtime_error("Path Resolver: Illegal path. Target does not exist");
    }

    current = storage_handler_.GetElement(next_id.value());
    if (!current->IsLive() || current->type_ != ElementType::FOLDER) {
      throw std::runtime_error("Path Resolver: Illegal path. Target does not exist");
    }
  }
  directory_cache_.RecordAccess(path.wstring(), current->element_id_);
  return current;
}

auto PathResolver::ResolveFolder(const std::filesystem::path& path)
    -> std::shared_ptr<SleeveFolder> {
  auto current = root_;
  for (const auto& part : path.lexically_normal().relative_path()) {
    const auto name = part.wstring();
    if (name.empty() || name == L".") {
      continue;
    }
    const auto next_id = current->GetElementIdByName(name);
    const auto next    = next_id.has_value() ? storage_handler_.GetElement(next_id.value())
                                             : std::shared_ptr<SleeveElement>{};
    if (!next || next->type_ != ElementType::FOLDER || !next->IsLive()) {
      throw std::runtime_error("Path Resolver: Illegal path. Target folder does not exist");
    }
    current = std::static_pointer_cast<SleeveFolder>(next);
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
  current->MarkModified();
  // directory_cache_.RecordAccess(path.wstring(), current->element_id_);
  return current;
}

auto PathResolver::CoWHandler(const std::shared_ptr<SleeveElement>& to_copy,
                              const std::shared_ptr<SleeveFolder>&  parent_folder)
    -> std::shared_ptr<SleeveElement> {
  auto old_id = to_copy->element_id_;
  // The copy is a new element with no stored rows, so it takes the members of the source: the
  // child folders from memory, and the file links from one read of the source (before any
  // change, so a failed read changes nothing).
  std::vector<sl_element_id_t> file_ids;
  if (to_copy->type_ == ElementType::FOLDER) {
    file_ids = storage_handler_.FileIds(static_cast<const SleeveFolder&>(*to_copy));
  }
  to_copy->DecrementRefCount();
  auto copied = to_copy->Copy(id_gen_.GenerateID());
  // Increment every child's ref count
  if (copied->type_ == ElementType::FOLDER) {
    auto  copied_folder = std::static_pointer_cast<SleeveFolder>(copied);
    auto& contents      = copied_folder->ListElements();
    for (auto& e : contents) {
      auto element = storage_handler_.GetElement(e);
      if (element && element->type_ == ElementType::FOLDER) {
        element->IncrementRefCount();
      }
    }
    for (const auto file_id : file_ids) {
      copied_folder->RecordFileLinked(file_id);
    }
  }
  parent_folder->UpdateElementMap(copied->element_name_, old_id, copied->element_id_);
  // A record of this path names the source, which stays live while another folder holds it.
  directory_cache_.Flush();
  copied->IncrementRefCount();
  storage_handler_.AddToStorage(copied);
  to_copy->MarkModified();
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

  auto       dfs_stack     = std::stack<TreeNode>();
  const auto push_children = [this, &dfs_stack](const SleeveFolder& folder, int depth) {
    auto        children      = storage_handler_.FileIds(folder);
    const auto& child_folders = folder.ListElements();
    children.insert(children.end(), child_folders.begin(), child_folders.end());
    std::sort(children.begin(), children.end(), std::greater<sl_element_id_t>{});
    for (auto& e : children) {
      dfs_stack.push({e, depth, false});
    }
  };

  auto visit_folder = std::static_pointer_cast<SleeveFolder>(start_node);
  push_children(*visit_folder, 0);

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
      push_children(static_cast<const SleeveFolder&>(*next_visit_element), next_visit.depth_ + 1);
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
