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
#include "utils/intrusive_list.hpp"

namespace alcedo {
enum class ElementType { FILE, FOLDER };

/// The state of an element against its database rows. UNSYNC: no row yet. MODIFIED: the row is
/// older than the element. SYNCED: the row matches. DELETED: the next sync removes the row.
enum class SyncFlag { UNSYNC, MODIFIED, SYNCED, DELETED };

// NodeCache holds elements and each element points back to its cache, so node_cache.hpp includes
// this header. The element uses the cache through this pointer only; sleeve_element.cpp includes
// node_cache.hpp.
class NodeCache;

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

  explicit SleeveElement(sl_element_id_t id, file_name_t element_name);
  // A NodeCache keeps the address of each element that it holds.
  SleeveElement(const SleeveElement&)                    = delete;
  auto operator=(const SleeveElement&) -> SleeveElement& = delete;

  virtual ~SleeveElement();

  virtual auto Copy(sl_element_id_t new_id) const -> std::shared_ptr<SleeveElement>;
  virtual auto Clear() -> bool;
  void         SetAddTime();
  void         SetLastModifiedTime();
  void         IncrementRefCount();
  void         DecrementRefCount();
  auto         IsShared() -> bool;

  /// The sync flag. Reads memory only.
  [[nodiscard]] auto SyncState() const -> SyncFlag { return sync_flag_; }
  /// True when the flag is not DELETED.
  [[nodiscard]] auto IsLive() const -> bool { return sync_flag_ != SyncFlag::DELETED; }
  /**
   * @brief The only writer of the sync flag.
   *
   * When a NodeCache holds the element, the cache moves the element into its dirty list
   * (UNSYNC, MODIFIED, DELETED) or out of it (SYNCED) in the same call. A file that becomes
   * SYNCED enters the eviction list of the cache.
   */
  void               SetSyncFlag(SyncFlag flag);
  /// SYNCED becomes MODIFIED; every other flag stays. Use after a change to a stored element.
  void               MarkModified();

 private:
  friend class NodeCache;

  SyncFlag                         sync_flag_ = SyncFlag::UNSYNC;
  /// The cache that holds the element, from NodeCache::Insert to NodeCache::Erase or the
  /// destruction of the cache. nullptr for an element that no cache holds.
  NodeCache*                       cache_     = nullptr;
  /// The link of the element in one list of its cache: the dirty list for a dirty element, the
  /// eviction list for a clean file, no list for a clean folder (NodeCache).
  IntrusiveListHook<SleeveElement> list_hook_;
};
};  // namespace alcedo
