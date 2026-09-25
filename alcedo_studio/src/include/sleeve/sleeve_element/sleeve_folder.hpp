//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "sleeve/sleeve_element/sleeve_element.hpp"
#include "sleeve/sleeve_filter/filter_combo.hpp"
#include "sleeve_element.hpp"
#include "type/type.hpp"

namespace alcedo {
/**
 * @brief A type of element that contains files or folders of its kind
 *
 */
class SleeveFolder : public SleeveElement {
 protected:
  std::unordered_map<file_name_t, sl_element_id_t>              contents_;
  std::unordered_map<filter_id_t, std::vector<sl_element_id_t>> indicies_cache_;
  filter_id_t                                                   default_filter_ = 0;

  uint32_t                                                      file_count_;
  uint32_t                                                      folder_count_;
  bool                                                          children_loaded_ = false;

  /// Child ids added to or removed from this folder since its `FolderContent` rows were last
  /// written. An id is in at most one of the two sets: removing an id that was added since the
  /// last write (or the reverse) cancels the pending change.
  std::unordered_set<sl_element_id_t>                           content_added_since_sync_;
  std::unordered_set<sl_element_id_t>                           content_removed_since_sync_;

  void RecordContentAdded(sl_element_id_t element_id);
  void RecordContentRemoved(sl_element_id_t element_id);

 public:
  explicit SleeveFolder(sl_element_id_t id, file_name_t element_name);
  ~SleeveFolder();

  auto Copy(sl_element_id_t new_id) const -> std::shared_ptr<SleeveElement>;

  void AddElementToMap(const std::shared_ptr<SleeveElement> element);
  void AddElementToMap(const std::shared_ptr<SleeveElement> element, bool change_sync);
  /// Add @p element as a child.
  /// @param change_sync  true for a membership change: the folder becomes MODIFIED and the id is
  ///                     recorded in ContentAddedSinceSync(). false only when the child is read
  ///                     from its existing `FolderContent` row (loading the folder).
  void AddElementToMap(const std::shared_ptr<SleeveElement> element, bool change_sync,
                       bool increment_ref_count);
  void ReplaceChild(const sl_element_id_t from, const sl_element_id_t to);
  void UpdateElementMap(const file_name_t& name, const sl_element_id_t old_id,
                        const sl_element_id_t new_id);
  auto GetElementIdByName(const file_name_t& name) const -> std::optional<sl_element_id_t>;
  auto ListElements() const -> const std::vector<sl_element_id_t>&;
  auto ContainsElementId(sl_element_id_t element_id) const -> bool;

  auto HasFilterIndex(const filter_id_t filter_id) const -> bool {
    return indicies_cache_.contains(filter_id);
  }

  auto ListElementsByFilter(const filter_id_t filter_id) const
      -> const std::vector<sl_element_id_t>&;
  auto Contains(const file_name_t& name) const -> bool;
  void RemoveNameFromMap(const file_name_t& name);
  auto RemoveElementById(sl_element_id_t element_id) -> bool;

  void CreateIndex(const std::vector<std::shared_ptr<SleeveElement>>& filtered_elements,
                   const filter_id_t                                  filter_id);

  void IncrementFolderCount();
  void IncrementFileCount();
  void DecrementFolderCount();
  void DecrementFileCount();
  auto Clear() -> bool;
  auto ResetFilters() -> bool;
  auto ContentSize() -> size_t;
  auto ChildrenLoaded() const -> bool { return children_loaded_; }
  void MarkChildrenLoaded(bool loaded = true) { children_loaded_ = loaded; }

  /// Child ids whose `FolderContent` rows the next sync inserts.
  auto ContentAddedSinceSync() const -> const std::unordered_set<sl_element_id_t>& {
    return content_added_since_sync_;
  }
  /// Child ids whose `FolderContent` rows the next sync deletes.
  auto ContentRemovedSinceSync() const -> const std::unordered_set<sl_element_id_t>& {
    return content_removed_since_sync_;
  }
  /// Clear the pending content changes. Call after the transaction that wrote them (or that
  /// inserted the folder with its full content list) committed.
  void MarkContentSynced();
};
};  // namespace alcedo
