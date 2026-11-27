//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <memory>
#include <vector>

#include "sleeve/node_cache.hpp"
#include "sleeve/sleeve_element/sleeve_element.hpp"
#include "sleeve/sleeve_element/sleeve_folder.hpp"
#include "storage/store/sleeve/element_store.hpp"
#include "storage/store/sleeve/folder_entry_store.hpp"
#include "type/type.hpp"

namespace alcedo {

/**
 * @brief Loads the elements that the NodeCache of a FileSystem does not hold.
 *
 * Each loaded element goes into the cache. An element that the cache holds keeps its live
 * object, so the changes of the current write stay visible. Not thread-safe: the FileSystem
 * owner serializes the calls.
 */
class NodeStorageHandler {
 public:
  NodeStorageHandler(ElementStore& element_store, FolderEntryStore& folder_entry_store,
                     NodeCache& nodes);

  /// Puts @p new_element into the cache (NodeCache::Insert).
  void AddToStorage(std::shared_ptr<SleeveElement> new_element);
  /// Load every child of @p folder into the node cache and the folder. A failed load writes one
  /// warning with the folder id and the error text and leaves the folder unloaded.
  void EnsureChildrenLoaded(const std::shared_ptr<SleeveFolder>& folder);
  /**
   * @brief The element @p id: the cached element, or the stored element, which then goes into
   *        the cache.
   * @throws The store error of ElementStore::GetElementById when no row has the id.
   */
  auto GetElement(sl_element_id_t id) -> std::shared_ptr<SleeveElement>;
  /**
   * @brief The live child folders of @p folder. Loads no file element.
   *
   * @details A folder with loaded children answers from memory, so the changes of the current
   * write are in the result. Otherwise one FolderEntryStore statement reads the child folders:
   * a folder that the node cache already holds keeps its live object, and each other folder goes
   * into the cache. The folder itself stays unloaded. Deleted folders are not in the result.
   * @throws The store error of FolderEntryStore::ReadChildFolders.
   */
  auto ChildFolders(const SleeveFolder& folder) -> std::vector<std::shared_ptr<SleeveFolder>>;

 private:
  ElementStore&     element_store_;
  FolderEntryStore& folder_entry_store_;
  NodeCache&        nodes_;
};

}  // namespace alcedo
