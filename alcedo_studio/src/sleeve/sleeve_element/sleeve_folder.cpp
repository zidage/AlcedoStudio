//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "sleeve/sleeve_element/sleeve_folder.hpp"

#include <algorithm>
#include <functional>
#include <span>
#include <memory>
#include <optional>
#include <stdexcept>
#include <unordered_set>
#include <vector>

#include "sleeve/sleeve_element/sleeve_element.hpp"
#include "type/type.hpp"

namespace alcedo {
void SleeveFolder::ChildFolders::Insert(const file_name_t& name, sl_element_id_t id) {
  by_name_.emplace(name, id);
  in_entry_order_.push_back(id);
  ids_.insert(id);
}

auto SleeveFolder::ChildFolders::Erase(const file_name_t& name) -> sl_element_id_t {
  const auto it = by_name_.find(name);
  if (it == by_name_.end()) {
    throw std::out_of_range("SleeveFolder: no child folder holds the name");
  }
  const auto id = it->second;
  by_name_.erase(it);
  std::erase(in_entry_order_, id);
  ids_.erase(id);
  return id;
}

void SleeveFolder::ChildFolders::Replace(const file_name_t& name, sl_element_id_t old_id,
                                         sl_element_id_t new_id) {
  by_name_[name] = new_id;
  if (const auto it = std::find(in_entry_order_.begin(), in_entry_order_.end(), old_id);
      it != in_entry_order_.end()) {
    *it = new_id;
  } else {
    in_entry_order_.push_back(new_id);
  }
  ids_.erase(old_id);
  ids_.insert(new_id);
}

void SleeveFolder::ChildFolders::Clear() {
  by_name_.clear();
  in_entry_order_.clear();
  ids_.clear();
}

auto SleeveFolder::ChildFolders::IdByName(const file_name_t& name) const
    -> std::optional<sl_element_id_t> {
  const auto it = by_name_.find(name);
  if (it == by_name_.end()) {
    return std::nullopt;
  }
  return it->second;
}

SleeveFolder::SleeveFolder(sl_element_id_t id, file_name_t element_name)
    : SleeveElement(id, element_name), file_count_(0), folder_count_(0) {
  type_ = ElementType::FOLDER;
}

SleeveFolder::~SleeveFolder() {}

auto SleeveFolder::Copy(sl_element_id_t new_id) const -> std::shared_ptr<SleeveElement> {
  auto copy            = std::make_shared<SleeveFolder>(new_id, element_name_);
  copy->child_folders_ = child_folders_;
  copy->mount_id_      = mount_id_;
  return copy;
}

void SleeveFolder::AddElementToMap(const std::shared_ptr<SleeveElement> element) {
  AddElementToMap(element, true, true);
}

void SleeveFolder::AddElementToMap(const std::shared_ptr<SleeveElement> element, bool change_sync) {
  AddElementToMap(element, change_sync, true);
}

void SleeveFolder::AddElementToMap(const std::shared_ptr<SleeveElement> element, bool change_sync,
                                   bool increment_ref_count) {
  if (element->type_ != ElementType::FOLDER) {
    throw std::invalid_argument("SleeveFolder: a folder holds no file entry; link the file");
  }
  if (auto existing = GetElementIdByName(element->element_name_); existing.has_value()) {
    if (existing.value() == element->element_id_) {
      return;
    }
    throw std::runtime_error("SleeveFolder: Name already exists in folder");
  }
  if (HoldsChildFolder(element->element_id_)) {
    return;
  }
  child_folders_.Insert(element->element_name_, element->element_id_);
  // Once a pinned element is added to the current folder, current folder also becomes pinned
  pinned_ |= element->pinned_;
  if (increment_ref_count) {
    element->IncrementRefCount();
  }
  if (!change_sync) {
    return;
  }
  RecordContentAdded(element->element_id_);
  // Mark this folder as modified so the content change is persisted on next sync.
  MarkModified();
}

void SleeveFolder::RestoreChildFolders(
    std::span<const sl_element_id_t> child_ids,
    const std::function<std::shared_ptr<SleeveElement>(sl_element_id_t)>& resolve) {
  child_folders_.Clear();
  indicies_cache_.clear();
  for (const auto id : child_ids) {
    const auto child = resolve(id);
    if (!child || child->element_id_ != id || child->type_ != ElementType::FOLDER ||
        !child->IsLive()) {
      throw std::runtime_error("SleeveFolder: Cannot restore an invalid child folder");
    }
    AddElementToMap(child, false, false);
  }
  MarkContentSynced();
}

void SleeveFolder::RecordContentAdded(sl_element_id_t element_id) {
  if (content_removed_since_sync_.erase(element_id) == 0) {
    content_added_since_sync_.insert(element_id);
  }
}

void SleeveFolder::RecordContentRemoved(sl_element_id_t element_id) {
  if (content_added_since_sync_.erase(element_id) == 0) {
    content_removed_since_sync_.insert(element_id);
  }
}

void SleeveFolder::RecordFileLinked(sl_element_id_t file_id) {
  content_removed_since_sync_.erase(file_id);
  content_added_since_sync_.insert(file_id);
  MarkModified();
}

void SleeveFolder::RecordFileUnlinked(sl_element_id_t file_id) {
  content_added_since_sync_.erase(file_id);
  content_removed_since_sync_.insert(file_id);
  MarkModified();
}

auto SleeveFolder::PendingMembership(sl_element_id_t element_id) const -> std::optional<bool> {
  if (content_added_since_sync_.contains(element_id)) {
    return true;
  }
  if (content_removed_since_sync_.contains(element_id)) {
    return false;
  }
  return std::nullopt;
}

void SleeveFolder::DropPendingChanges(const std::unordered_set<sl_element_id_t>& element_ids) {
  // The smaller set drives the walk: a delete of a few files in a large pending import, or a
  // delete of many files with few pending changes.
  const auto drop_from = [&element_ids](std::unordered_set<sl_element_id_t>& pending) {
    if (pending.size() <= element_ids.size()) {
      std::erase_if(pending,
                    [&element_ids](sl_element_id_t id) { return element_ids.contains(id); });
      return;
    }
    for (const auto id : element_ids) {
      pending.erase(id);
    }
  };
  drop_from(content_added_since_sync_);
  drop_from(content_removed_since_sync_);
}

void SleeveFolder::MarkContentSynced() {
  content_added_since_sync_.clear();
  content_removed_since_sync_.clear();
}

void SleeveFolder::UpdateElementMap(const file_name_t& name, const sl_element_id_t old_id,
                                    const sl_element_id_t new_id) {
  child_folders_.Replace(name, old_id, new_id);
  RecordContentRemoved(old_id);
  RecordContentAdded(new_id);
  // Mark this folder as modified so the updated content list is persisted on next sync.
  MarkModified();
}

auto SleeveFolder::GetElementIdByName(const file_name_t& name) const
    -> std::optional<sl_element_id_t> {
  return child_folders_.IdByName(name);
}

auto SleeveFolder::ListElements() const -> const std::vector<sl_element_id_t>& {
  return child_folders_.InEntryOrder();
}

auto SleeveFolder::HoldsChildFolder(sl_element_id_t folder_id) const -> bool {
  return child_folders_.ContainsId(folder_id);
}

auto SleeveFolder::Clear() -> bool {
  for (const auto id : child_folders_.InEntryOrder()) {
    RecordContentRemoved(id);
  }
  child_folders_.Clear();
  indicies_cache_.clear();
  return true;
}

auto SleeveFolder::Contains(const file_name_t& name) const -> bool {
  return child_folders_.ContainsName(name);
}

void SleeveFolder::RemoveNameFromMap(const file_name_t& name) {
  const auto removed_id = child_folders_.Erase(name);
  RecordContentRemoved(removed_id);
  // Mark this folder as modified so the updated content list is persisted on next sync.
  MarkModified();
}

void SleeveFolder::CreateIndex(const std::vector<std::shared_ptr<SleeveElement>>& filtered_elements,
                               const filter_id_t                                  filter_id) {
  std::vector<sl_element_id_t> new_index;
  for (const auto& element : filtered_elements) {
    new_index.push_back(element->element_id_);
  }
  indicies_cache_[filter_id] = new_index;
}

auto SleeveFolder::ListElementsByFilter(const filter_id_t filter_id) const
    -> const std::vector<sl_element_id_t>& {
  if (!HasFilterIndex(filter_id)) {
    throw std::runtime_error("Filter index not found in folder.");
  }
  return indicies_cache_.at(filter_id);
}

void SleeveFolder::IncrementFileCount() { ++file_count_; }

void SleeveFolder::DecrementFileCount() { --file_count_; }

void SleeveFolder::IncrementFolderCount() { ++folder_count_; }

void SleeveFolder::DecrementFolderCount() { --folder_count_; }
};  // namespace alcedo
