//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "sleeve/sleeve_element/sleeve_folder.hpp"

#include <algorithm>
#include <memory>
#include <optional>
#include <stdexcept>
#include <unordered_set>
#include <vector>

#include "sleeve/sleeve_element/sleeve_element.hpp"
#include "type/type.hpp"

namespace alcedo {
SleeveFolder::SleeveFolder(sl_element_id_t id, file_name_t element_name)
    : SleeveElement(id, element_name), file_count_(0), folder_count_(0) {
  indicies_cache_[default_filter_] = {};
  type_                            = ElementType::FOLDER;
}

SleeveFolder::~SleeveFolder() {}

auto SleeveFolder::Copy(sl_element_id_t new_id) const -> std::shared_ptr<SleeveElement> {
  auto copy       = std::make_shared<SleeveFolder>(new_id, element_name_);
  copy->contents_ = {contents_};
  copy->indicies_cache_[copy->default_filter_] =
      std::vector<sl_element_id_t>(indicies_cache_.at(default_filter_));
  copy->mount_id_ = mount_id_;
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
  contents_[element->element_name_] = element->element_id_;
  indicies_cache_[default_filter_].push_back(element->element_id_);
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
  contents_.erase(name);
  contents_[name]      = new_id;
  auto& default_filter = indicies_cache_[default_filter_];

  for (auto& id : default_filter) {
    if (id == old_id) {
      id = new_id;
      break;
    }
  }
  RecordContentRemoved(old_id);
  RecordContentAdded(new_id);
  // Mark this folder as modified so the updated content list is persisted on next sync.
  MarkModified();
}

auto SleeveFolder::GetElementIdByName(const file_name_t& name) const
    -> std::optional<sl_element_id_t> {
  const auto it = contents_.find(name);
  if (it == contents_.end()) {
    return std::nullopt;
  }
  return it->second;
}

auto SleeveFolder::ListElements() const -> const std::vector<sl_element_id_t>& {
  return indicies_cache_.at(default_filter_);
}

auto SleeveFolder::HoldsChildFolder(sl_element_id_t folder_id) const -> bool {
  const auto& child_folders = indicies_cache_.at(default_filter_);
  return std::find(child_folders.begin(), child_folders.end(), folder_id) != child_folders.end();
}

auto SleeveFolder::Clear() -> bool {
  for (const auto id : indicies_cache_[default_filter_]) {
    RecordContentRemoved(id);
  }
  contents_.clear();
  indicies_cache_.clear();
  indicies_cache_[default_filter_] = {};
  return true;
}

auto SleeveFolder::Contains(const file_name_t& name) const -> bool {
  return contents_.contains(name);
}

void SleeveFolder::RemoveNameFromMap(const file_name_t& name) {
  auto  removed_id    = contents_.at(name);
  auto& default_index = indicies_cache_[default_filter_];
  default_index.erase(std::remove(default_index.begin(), default_index.end(), removed_id),
                      default_index.end());
  contents_.erase(name);
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
