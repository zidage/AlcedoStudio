//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <span>
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
 * @brief A folder of the sleeve file system.
 *
 * @details The folder holds the names and ids of its child folders in memory. A folder name is
 * unique among the child folders of one parent. File membership is not in memory: it is the
 * `FolderContent` rows of the folder plus the pending sets (Sleeve Node Cache Plan, SN4). A file
 * name is a label with no uniqueness rule.
 */
class SleeveFolder : public SleeveElement {
 private:
  /**
   * @brief The child folders of one folder: by name, in the order of their entries, and as an id
   *        set. Only the functions of this class change the three members, so they hold the same
   *        folders.
   */
  class ChildFolders {
   public:
    /// Adds @p id under @p name. @pre No child folder holds @p name or @p id.
    void Insert(const file_name_t& name, sl_element_id_t id);
    /// Removes the child folder named @p name and returns its id. @pre A child folder holds it.
    auto Erase(const file_name_t& name) -> sl_element_id_t;
    /// Puts @p new_id under @p name in place of @p old_id, at the same entry position.
    void Replace(const file_name_t& name, sl_element_id_t old_id, sl_element_id_t new_id);
    void Clear();

    auto IdByName(const file_name_t& name) const -> std::optional<sl_element_id_t>;
    auto ContainsName(const file_name_t& name) const -> bool { return by_name_.contains(name); }
    auto ContainsId(sl_element_id_t id) const -> bool { return ids_.contains(id); }
    auto InEntryOrder() const -> const std::vector<sl_element_id_t>& { return in_entry_order_; }

   private:
    std::unordered_map<file_name_t, sl_element_id_t> by_name_;
    std::vector<sl_element_id_t>                     in_entry_order_;
    std::unordered_set<sl_element_id_t>              ids_;
  };

  ChildFolders child_folders_;

 protected:
  /// The file ids of each filter result (ApplyFilterToFolder), by filter id.
  std::unordered_map<filter_id_t, std::vector<sl_element_id_t>> indicies_cache_;

  uint32_t                                                      file_count_;
  uint32_t                                                      folder_count_;

  /// Child ids whose `FolderContent` rows the next sync inserts or deletes. An id is in at most
  /// one of the two sets. For a child folder, a remove of an id that was added since the last
  /// write (or the reverse) cancels the pending change, because memory holds every child folder.
  /// For a file, the last link or unlink wins: the insert ignores an existing row and the delete
  /// ignores a missing row, so no change needs a read.
  std::unordered_set<sl_element_id_t>                           content_added_since_sync_;
  std::unordered_set<sl_element_id_t>                           content_removed_since_sync_;

  void RecordContentAdded(sl_element_id_t element_id);
  void RecordContentRemoved(sl_element_id_t element_id);

 public:
  explicit SleeveFolder(sl_element_id_t id, file_name_t element_name);
  ~SleeveFolder();

  /// A new folder with the id @p new_id, the name, the mount id, and the child folders of this
  /// folder. The file links are not copied: PathResolver::CoWHandler records them on the copy.
  auto Copy(sl_element_id_t new_id) const -> std::shared_ptr<SleeveElement>;

  void AddElementToMap(const std::shared_ptr<SleeveElement> element);
  void AddElementToMap(const std::shared_ptr<SleeveElement> element, bool change_sync);
  /**
   * @brief Adds the folder @p element as a child folder.
   * @param change_sync  true for a membership change: the folder becomes MODIFIED and the id is
   *                     recorded in ContentAddedSinceSync(). false only when the child is read
   *                     from its existing `FolderContent` row (NodeStorageHandler::LoadFolderTree).
   * @throws std::invalid_argument when @p element is a file (use RecordFileLinked), and
   *         std::runtime_error when another child folder holds the name.
   */
  void AddElementToMap(const std::shared_ptr<SleeveElement> element, bool change_sync,
                       bool increment_ref_count);
  /// Restores committed child folders in entry order, preserving their reference counts and
  /// sync flags. Clears pending membership and cached filter results. The caller holds the
  /// FileSystem lock and must prevent further access if restoration throws.
  void RestoreChildFolders(
      std::span<const sl_element_id_t> child_ids,
      const std::function<std::shared_ptr<SleeveElement>(sl_element_id_t)>& resolve);
  void ReplaceChild(const sl_element_id_t from, const sl_element_id_t to);
  /// Replaces the child folder @p old_id under @p name with @p new_id (copy-on-write).
  void UpdateElementMap(const file_name_t& name, const sl_element_id_t old_id,
                        const sl_element_id_t new_id);
  /// The id of the child folder named @p name. Reads memory only.
  auto GetElementIdByName(const file_name_t& name) const -> std::optional<sl_element_id_t>;
  /// The ids of the child folders, in the order of their entries.
  auto ListElements() const -> const std::vector<sl_element_id_t>&;
  /// True when @p folder_id is a child folder of this folder. Reads memory only.
  auto HoldsChildFolder(sl_element_id_t folder_id) const -> bool;

  auto HasFilterIndex(const filter_id_t filter_id) const -> bool {
    return indicies_cache_.contains(filter_id);
  }

  auto ListElementsByFilter(const filter_id_t filter_id) const
      -> const std::vector<sl_element_id_t>&;
  /// True when a child folder holds @p name.
  auto Contains(const file_name_t& name) const -> bool;
  /// Removes the child folder named @p name. The folder becomes MODIFIED.
  void RemoveNameFromMap(const file_name_t& name);

  /**
   * @brief Records a link of the file @p file_id: the next sync inserts its row, or keeps the
   *        existing row. Cancels a pending unlink of the file. The folder becomes MODIFIED.
   */
  void RecordFileLinked(sl_element_id_t file_id);
  /**
   * @brief Records an unlink of the file @p file_id: the next sync deletes its row. Cancels a
   *        pending link of the file. The folder becomes MODIFIED.
   * @pre The caller checked that the file is a member (FolderHoldsFile in the plan, section 3.3).
   */
  void RecordFileUnlinked(sl_element_id_t file_id);
  /// The pending change of @p element_id: true for a link, false for an unlink, empty for no
  /// change since the last sync. Reads memory only.
  auto PendingMembership(sl_element_id_t element_id) const -> std::optional<bool>;
  /// Forgets the pending changes of the ids in @p element_ids. For files that the same write
  /// deletes: the delete of the files removes their rows. The sync flag stays.
  void DropPendingChanges(const std::unordered_set<sl_element_id_t>& element_ids);

  void CreateIndex(const std::vector<std::shared_ptr<SleeveElement>>& filtered_elements,
                   const filter_id_t                                  filter_id);

  void IncrementFolderCount();
  void IncrementFileCount();
  void DecrementFolderCount();
  void DecrementFileCount();
  auto Clear() -> bool;
  auto ResetFilters() -> bool;

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
