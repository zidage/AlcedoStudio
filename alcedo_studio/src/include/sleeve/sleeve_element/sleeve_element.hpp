//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <ctime>
#include <memory>
#include <optional>
#include <string>

#include "sleeve/sleeve_filter/filter_combo.hpp"
#include "type/type.hpp"

namespace alcedo {
enum class ElementType { FILE, FOLDER };

enum class SyncFlag { UNSYNC, MODIFIED, SYNCED, DELETED };
/**
 * @brief Abstract objects residing in a sleeve, it can be files or folders
 *
 */
class SleeveElement {
 public:
  sl_element_id_t element_id_;
  ElementType     type_;

  file_name_t     element_name_;

  std::time_t     added_time_;
  std::time_t     last_modified_time_;

  uint32_t        ref_count_;
  bool            pinned_;

  /// Id of the mount that holds this element (`Element.mount_id`). Empty for an ordinary
  /// element. Copy() keeps the value, because a copy-on-write copy replaces the element at
  /// the same path; FileSystem::DuplicateFileToFolder clears it on the new photo.
  std::optional<mount_id_t> mount_id_;

  SyncFlag        sync_flag_ = SyncFlag::UNSYNC;

  explicit SleeveElement(sl_element_id_t id, file_name_t element_name);

  virtual ~SleeveElement();

  virtual auto Copy(sl_element_id_t new_id) const -> std::shared_ptr<SleeveElement>;
  virtual auto Clear() -> bool;
  void         SetAddTime();
  void         SetLastModifiedTime();
  void         IncrementRefCount();
  void         DecrementRefCount();
  auto         IsShared() -> bool;
  void         SetSyncFlag(SyncFlag flag);

  // virtual void AddElement(std::shared_ptr<SleeveElement>);
  // virtual void CreateFilter(FilterCombo&& filter);
  // virtual auto GetElementByName(file_name_t name) -> std::optional<sl_element_id_t>;
  // virtual auto ListElements() -> std::vector<sl_element_id_t>;
  // virtual auto RecursiveListElements() -> std::vector<sl_element_id_t>;
  // virtual auto RemoveElementByName(file_name_t name) -> sl_element_id_t;
};
};  // namespace alcedo
