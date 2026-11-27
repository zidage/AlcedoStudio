//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_set>
#include <vector>

#include "sleeve/sleeve_element/sleeve_element.hpp"
#include "sleeve/sleeve_element/sleeve_folder.hpp"
#include "type/type.hpp"
#include "utils/intrusive_list.hpp"

namespace alcedo {

/**
 * @brief The in-memory nodes of one sleeve file system (Sleeve Node Cache Plan, SN2).
 *
 * A page table indexed by the element id holds the nodes: page `id / kPageSlots` holds slot
 * `id % kPageSlots`. A page is allocated at the first insert into its id range and released when
 * its last slot empties, so a lookup has no hash and the memory follows the used id ranges.
 *
 * Each element is in at most one of two lists, through its one list hook:
 * - The dirty list holds each element whose sync flag is UNSYNC, MODIFIED, or DELETED.
 * - The eviction list holds each clean (SYNCED) file element, least recently used first
 *   (Sleeve Node Cache Plan, SN7).
 * A clean folder is in no list: all folders are resident. SleeveElement::SetSyncFlag moves an
 * element between the lists through the back pointer that Insert sets, so both lists are complete
 * without a scan of the table.
 *
 * EvictToLimit removes the least recently used clean files that no caller holds until the
 * eviction list holds at most the limit. An element that a caller holds stays, so a caller that
 * keeps an element keeps its identity: the next Find gives the same object.
 *
 * The cache reads no database. Not thread-safe: SleeveServiceImpl serializes every call under
 * its file system lock.
 */
class NodeCache {
 public:
  /// Slots in one page of the page table (64 KiB of pointers).
  static constexpr std::size_t kPageSlots        = 4096;
  /**
   * @brief The default limit of clean file elements (Sleeve Node Cache Plan, SN7).
   *
   * SN6 measured 207 heap bytes for one file element with no Image (Release), so 262,144
   * elements use about 64 MB with the heap block headers.
   */
  static constexpr std::size_t kCleanFileLimit   = 262'144;

  NodeCache()                                    = default;
  NodeCache(const NodeCache&)                    = delete;
  auto operator=(const NodeCache&) -> NodeCache& = delete;
  /// Detaches each element that the cache still holds, so an element that a caller keeps after
  /// the cache is gone changes its own flag only.
  ~NodeCache();

  /**
   * @brief The element @p id, or nullptr when the cache does not hold it. Reads memory only.
   *
   * A clean file moves to the back of the eviction list (most recently used).
   */
  [[nodiscard]] auto Find(sl_element_id_t id) -> std::shared_ptr<SleeveElement>;
  /// True when the cache holds the element @p id. Reads memory only.
  [[nodiscard]] auto Contains(sl_element_id_t id) const -> bool;

  /**
   * @brief Puts @p element into the slot of its id and sets its back pointer. A dirty element
   *        enters the dirty list; a clean file enters the back of the eviction list.
   * @throws std::invalid_argument when @p element is null.
   * @throws std::logic_error when another cache holds @p element, or when the slot holds a
   *         different element.
   *         Inserting the element that the slot already holds changes nothing.
   */
  void               Insert(std::shared_ptr<SleeveElement> element);

  /**
   * @brief Removes the element @p id and clears its back pointer. An id that the cache does not
   *        hold changes nothing.
   * @throws std::logic_error when the element is dirty and its flag is not DELETED: the next
   *         sync would lose the change.
   */
  void               Erase(sl_element_id_t id);

  /// Calls @p visit with each folder that the cache holds, deleted folders included. @p visit can
  /// change the folders and their flags; it must not insert or erase elements.
  template <typename Visit>
  void ForEachFolder(Visit&& visit) const {
    for (const auto id : folder_ids_) {
      visit(static_cast<SleeveFolder&>(*Slot(id)));
    }
  }

  /// The elements of the dirty list in the order they became dirty.
  [[nodiscard]] auto DirtyElements() const -> std::vector<std::shared_ptr<SleeveElement>>;
  /// The number of elements in the dirty list.
  [[nodiscard]] auto DirtyCount() const -> std::size_t { return dirty_.Size(); }
  /// The number of clean file elements (the eviction list), held by a caller or not.
  [[nodiscard]] auto CleanFileCount() const -> std::size_t { return clean_files_.Size(); }
  /// The number of elements that the cache holds.
  [[nodiscard]] auto Size() const -> std::size_t { return size_; }
  /// The number of allocated pages.
  [[nodiscard]] auto PageCount() const -> std::size_t { return page_count_; }

  /// The largest number of clean file elements that EvictToLimit keeps.
  [[nodiscard]] auto Limit() const -> std::size_t { return limit_; }
  /// Sets the limit of EvictToLimit. Evicts nothing: the next EvictToLimit applies it.
  void               SetLimit(std::size_t limit) { limit_ = limit; }

  /**
   * @brief Removes clean file elements, least recently used first, until the eviction list
   *        holds at most Limit() elements. Returns the number of removed elements.
   *
   * @details An element that only the cache holds (`use_count() == 1`) leaves the cache, and its
   * back pointer is cleared. An element that a caller holds stays and moves to the back. The walk
   * visits each element of the list at most once, so when callers hold more than Limit() clean
   * files the list stays above the limit. A folder and a dirty element are never in the list.
   * @pre The caller holds the lock that serializes the cache (SleeveServiceImpl::fs_lock_). A
   *      reference to a cached element is created only under that lock (Find, or a copy of a
   *      reference that a caller already holds), so a count of 1 stays 1 during the walk.
   */
  auto               EvictToLimit() -> std::size_t;

 private:
  friend class SleeveElement;

  using Page = std::array<std::shared_ptr<SleeveElement>, kPageSlots>;
  using ElementList = IntrusiveList<SleeveElement, &SleeveElement::list_hook_>;

  struct PageEntry {
    std::unique_ptr<Page> elements;
    std::uint32_t         used = 0;
  };

  /// SleeveElement::SetSyncFlag calls this after the flag of @p element changed from
  /// @p old_flag. Moves the element to the list of its new flag.
  void                      OnSyncFlagChanged(SleeveElement& element, SyncFlag old_flag);
  /// The list that holds an element of type @p type with the flag @p flag, or nullptr.
  [[nodiscard]] auto        ListFor(ElementType type, SyncFlag flag) -> ElementList*;
  /// The slot of @p id; the page must exist.
  [[nodiscard]] auto        Slot(sl_element_id_t id) const -> const std::shared_ptr<SleeveElement>&;
  /// Removes the element @p id from its list, the folder ids, and its page, and clears its back
  /// pointer. The cache must hold the element.
  void                      Release(sl_element_id_t id);
  [[nodiscard]] static auto IsDirty(SyncFlag flag) -> bool { return flag != SyncFlag::SYNCED; }

  std::vector<PageEntry>    pages_;
  std::size_t               page_count_ = 0;
  std::size_t               size_       = 0;
  std::size_t               limit_      = kCleanFileLimit;
  ElementList               dirty_;
  /// The clean file elements, least recently used first.
  ElementList               clean_files_;
  /// The ids of the held folders. A project has some thousands of folders, so a walk of all
  /// folders does not read the elements of the files.
  std::unordered_set<sl_element_id_t> folder_ids_;
};

}  // namespace alcedo
