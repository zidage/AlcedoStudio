//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <filesystem>
#include <memory>
#include <vector>

#include "sleeve/node_cache.hpp"
#include "sleeve/sleeve_element/sleeve_element.hpp"
#include "sleeve/sleeve_element/sleeve_folder.hpp"
#include "storage/store/sleeve/element_store.hpp"
#include "storage/store/sleeve/folder_entry_store.hpp"
#include "type/type.hpp"

namespace alcedo {

/// One folder of the folder tree at one path (Sleeve Node Cache Plan, SN3).
struct FolderTreeEntry {
  /// The path of the folder from the library root, for example `/Trip/Day 1`.
  std::filesystem::path         path_;
  /// The live folder that the node cache holds.
  std::shared_ptr<SleeveFolder> folder_;
};

/**
 * @brief Loads the elements that the NodeCache of a FileSystem does not hold.
 *
 * @details Each loaded element goes into the cache. An element that the cache holds keeps its
 * live object, so the changes of the current write stay visible. All folders are resident:
 * LoadFolderTree puts each folder into the cache when the project opens, and each folder then
 * holds the names and ids of its child folders. File entries of a folder load on demand
 * (EnsureChildrenLoaded). Not thread-safe: the FileSystem owner serializes the calls.
 */
class NodeStorageHandler {
 public:
  NodeStorageHandler(ElementStore& element_store, FolderEntryStore& folder_entry_store,
                     NodeCache& nodes);

  /// Puts @p new_element into the cache (NodeCache::Insert).
  void AddToStorage(std::shared_ptr<SleeveElement> new_element);
  /**
   * @brief Loads every stored folder into the cache and gives each folder its child folders,
   *        with one FolderEntryStore::ReadFolderTree call.
   *
   * @details Call once, after the root is in the cache and before any other operation. A folder
   * with more than one parent gets one child entry in each parent; the load does not change a
   * reference count and marks no folder modified. The walk from the root, and then from each
   * folder that the root does not reach, uses an explicit stack. A link to a folder on the
   * current walk path closes a cycle: the load skips the link and writes one warning with both
   * ids. A link whose child name the parent already holds for another folder is skipped with one
   * warning. The file entries of each folder stay unloaded.
   * @throws The store error of FolderEntryStore::ReadFolderTree, after one error log line with
   *         its text; the cache then holds the root only.
   */
  void LoadFolderTree();
  /// Load the file entries of @p folder into the node cache and the folder. The child folders
  /// are resident (LoadFolderTree), so a stored link to a folder adds nothing, and a file whose
  /// removal no sync wrote stays removed. A failed load writes one warning with the folder id and
  /// the error text and leaves the folder unloaded.
  void EnsureChildrenLoaded(const std::shared_ptr<SleeveFolder>& folder);
  /**
   * @brief The element @p id: the cached element, or the stored element, which then goes into
   *        the cache.
   * @throws The store error of ElementStore::GetElementById when no row has the id.
   */
  auto GetElement(sl_element_id_t id) -> std::shared_ptr<SleeveElement>;
  /**
   * @brief The live child folders of @p folder, in the order of the folder entries. Reads memory
   *        only: each folder is resident.
   */
  auto ChildFolders(const SleeveFolder& folder) const -> std::vector<std::shared_ptr<SleeveFolder>>;
  /**
   * @brief Every live folder below @p root at each of its paths, parents before children, in
   *        the order of the folder entries. Reads memory only.
   *
   * @details A folder with two parents is in the result once under each parent. The walk uses
   * an explicit stack and skips a child folder that is on the current walk path, so a cycle that
   * a write created ends the walk of that branch. @p root itself is not in the result; the paths
   * start at `/`.
   */
  auto FolderTree(const SleeveFolder& root) const -> std::vector<FolderTreeEntry>;

 private:
  ElementStore&     element_store_;
  FolderEntryStore& folder_entry_store_;
  NodeCache&        nodes_;
};

}  // namespace alcedo
