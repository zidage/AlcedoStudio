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
 * The dirty list holds each element whose sync flag is UNSYNC, MODIFIED, or DELETED.
 * SleeveElement::SetSyncFlag moves an element into and out of the list through the back pointer
 * that Insert sets, so the list is complete without a scan of the table.
 *
 * The cache reads no database. Not thread-safe: SleeveServiceImpl serializes every call under
 * its file system lock.
 */
class NodeCache {
 public:
  /// Slots in one page of the page table (64 KiB of pointers).
  static constexpr std::size_t kPageSlots        = 4096;

  NodeCache()                                    = default;
  NodeCache(const NodeCache&)                    = delete;
  auto operator=(const NodeCache&) -> NodeCache& = delete;
  /// Detaches each element that the cache still holds, so an element that a caller keeps after
  /// the cache is gone changes its own flag only.
  ~NodeCache();

  /// The element @p id, or nullptr when the cache does not hold it. Reads memory only.
  [[nodiscard]] auto Find(sl_element_id_t id) const -> std::shared_ptr<SleeveElement>;
  /// True when the cache holds the element @p id. Reads memory only.
  [[nodiscard]] auto Contains(sl_element_id_t id) const -> bool;

  /**
   * @brief Puts @p element into the slot of its id and sets its back pointer. A dirty element
   *        enters the dirty list.
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
  /// The number of elements that the cache holds.
  [[nodiscard]] auto Size() const -> std::size_t { return size_; }
  /// The number of allocated pages.
  [[nodiscard]] auto PageCount() const -> std::size_t { return page_count_; }

 private:
  friend class SleeveElement;

  using Page = std::array<std::shared_ptr<SleeveElement>, kPageSlots>;

  struct PageEntry {
    std::unique_ptr<Page> elements;
    std::uint32_t         used = 0;
  };

  /// SleeveElement::SetSyncFlag calls this after the flag of @p element changed.
  void                      OnSyncFlagChanged(SleeveElement& element);
  /// The slot of @p id; the page must exist.
  [[nodiscard]] auto        Slot(sl_element_id_t id) const -> const std::shared_ptr<SleeveElement>&;
  [[nodiscard]] static auto IsDirty(const SleeveElement& element) -> bool;

  std::vector<PageEntry>    pages_;
  std::size_t               page_count_ = 0;
  std::size_t               size_       = 0;
  IntrusiveList<SleeveElement, &SleeveElement::dirty_hook_> dirty_;
  /// The ids of the held folders. A project has some thousands of folders, so a walk of all
  /// folders does not read the elements of the files.
  std::unordered_set<sl_element_id_t>                       folder_ids_;
};

}  // namespace alcedo
