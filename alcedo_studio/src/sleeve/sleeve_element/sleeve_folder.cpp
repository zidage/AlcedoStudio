//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "sleeve/sleeve_element/sleeve_folder.hpp"

#include <algorithm>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <type_traits>
#include <vector>

#include "sleeve/sleeve_element/sleeve_element.hpp"
#include "sleeve/sleeve_element/sleeve_element_factory.hpp"
#include "sleeve/sleeve_filter/filter_combo.hpp"
#include "type/type.hpp"
#include "utils/string/convert.hpp"

namespace alcedo {
/**
 * @brief Construct a new Sleeve Folder:: Sleeve Folder object
 *
 * @param id
 * @param element_name
 */
SleeveFolder::SleeveFolder(sl_element_id_t id, file_name_t element_name)
    : SleeveElement(id, element_name), file_count_(0), folder_count_(0) {
  indicies_cache_[default_filter_] = {};
  type_                            = ElementType::FOLDER;
}

SleeveFolder::~SleeveFolder() {}

auto SleeveFolder::Copy(sl_element_id_t new_id) const -> std::shared_ptr<SleeveElement> {
  auto copy       = std::make_shared<SleeveFolder>(new_id, element_name_);
  // Copy the name-id map and filter map
  copy->contents_ = {contents_};
  // Only copy indicies cache under default filter
  copy->indicies_cache_[copy->default_filter_] =
      std::vector<sl_element_id_t>(indicies_cache_.at(default_filter_));
  copy->children_loaded_ = children_loaded_;
  return copy;
}
/**
 * @brief Add an element reference to the folder
 *
 * @param element
 */
void SleeveFolder::AddElementToMap(const std::shared_ptr<SleeveElement> element) {
  AddElementToMap(element, true, true);
}

void SleeveFolder::AddElementToMap(const std::shared_ptr<SleeveElement> element, bool change_sync) {
  AddElementToMap(element, change_sync, true);
}

void SleeveFolder::AddElementToMap(const std::shared_ptr<SleeveElement> element, bool change_sync,
                                   bool increment_ref_count) {
  if (auto existing = GetElementIdByName(element->element_name_); existing.has_value()) {
    if (existing.value() == element->element_id_) {
      return;
    }
    throw std::runtime_error("SleeveFolder: Name already exists in folder");
  }
  if (ContainsElementId(element->element_id_)) {
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
  if (sync_flag_ == SyncFlag::SYNCED) {
    sync_flag_ = SyncFlag::MODIFIED;
  }
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

void SleeveFolder::MarkContentSynced() {
  content_added_since_sync_.clear();
  content_removed_since_sync_.clear();
}

/**
 * @brief Update a name-id mapping
 *
 * @param name
 * @param old_id
 * @param new_id
 */
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
  if (sync_flag_ == SyncFlag::SYNCED) {
    sync_flag_ = SyncFlag::MODIFIED;
  }
}

/**
 * @brief Get an element's id from the _contents table
 *
 * @param name
 * @return std::optional<sl_element_id_t>
 */
auto SleeveFolder::GetElementIdByName(const file_name_t& name) const
    -> std::optional<sl_element_id_t> {
  if (!Contains(name)) {
    return std::nullopt;
  }
  return contents_.at(name);
}

/**
 * @brief List all the elements within this folder
 *
 * @return std::shared_ptr<std::vector<sl_element_id_t>>
 */
auto SleeveFolder::ListElements() const -> const std::vector<sl_element_id_t>& {
  const auto& default_list = indicies_cache_.at(default_filter_);
  return default_list;
}

auto SleeveFolder::ContainsElementId(sl_element_id_t element_id) const -> bool {
  const auto& default_list = indicies_cache_.at(default_filter_);
  return std::find(default_list.begin(), default_list.end(), element_id) != default_list.end();
}

auto SleeveFolder::Clear() -> bool {
  // TODO: Add Implementation
  for (const auto id : indicies_cache_[default_filter_]) {
    RecordContentRemoved(id);
  }
  contents_.clear();
  indicies_cache_.clear();
  indicies_cache_[default_filter_] = {};
  children_loaded_                 = true;

  return true;
}

/**
 * @brief Check whether the folder contains the element of the given name
 *
 * @param name
 * @return true
 * @return false
 */
auto SleeveFolder::Contains(const file_name_t& name) const -> bool {
  return contents_.count(name) != 0;
}

/**
 * @brief Remove a name-id mapping from the _contents table
 *
 * @param name
 * @return sl_element_id_t
 */
void SleeveFolder::RemoveNameFromMap(const file_name_t& name) {
  auto  removed_id    = contents_.at(name);
  // Also remove from default filter index
  auto& default_index = indicies_cache_[default_filter_];
  default_index.erase(std::remove(default_index.begin(), default_index.end(), removed_id),
                      default_index.end());
  contents_.erase(name);
  RecordContentRemoved(removed_id);
  // Mark this folder as modified so the updated content list is persisted on next sync.
  if (sync_flag_ == SyncFlag::SYNCED) {
    sync_flag_ = SyncFlag::MODIFIED;
  }
}

auto SleeveFolder::RemoveElementById(sl_element_id_t element_id) -> bool {
  auto removed = false;
  for (auto it = contents_.begin(); it != contents_.end();) {
    if (it->second == element_id) {
      it      = contents_.erase(it);
      removed = true;
    } else {
      ++it;
    }
  }

  auto& default_index = indicies_cache_[default_filter_];
  const auto old_size = default_index.size();
  default_index.erase(std::remove(default_index.begin(), default_index.end(), element_id),
                      default_index.end());
  removed |= default_index.size() != old_size;

  if (removed) {
    RecordContentRemoved(element_id);
  }
  if (removed && sync_flag_ == SyncFlag::SYNCED) {
    sync_flag_ = SyncFlag::MODIFIED;
  }
  return removed;
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
auto SleeveFolder::ContentSize() -> size_t { return contents_.size(); }
};  // namespace alcedo
