//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <memory>
#include <utility>
#include <vector>

#include "sleeve/sleeve_element/sleeve_folder.hpp"
#include "storage/store/database.hpp"
#include "type/type.hpp"

namespace alcedo {

/// The stored folder structure: every folder element and every folder-to-folder link.
struct FolderTreeRows {
  /// Every `Element` row of type FOLDER, in id order, with sync flag SYNCED. New objects; their
  /// children are not loaded.
  std::vector<std::shared_ptr<SleeveFolder>>               folders_;
  /// Every `FolderContent` row `(parent_id, child_id)` whose two ends are folder elements,
  /// ordered by parent id and then by child id.
  std::vector<std::pair<sl_element_id_t, sl_element_id_t>> edges_;
};

/**
 * @brief Reads of the folder structure (`Element` joined with `FolderContent`) that load no
 *        file element (Sleeve Node Cache Plan, SN1 and SN3).
 *
 * @details The store has no state. Each operation takes the connection and the database lock of
 * the Database owner for the whole operation, so it is safe to call from any thread. A
 * `FolderContent` row whose element has no `Element` row is not in a result.
 */
class FolderEntryStore {
 public:
  explicit FolderEntryStore(Database& database);

  /**
   * @brief Every folder element and every folder-to-folder link of the project, read under one
   *        database lock with one statement for the folders and one for the links.
   *
   * @details The result has no file element and no link to a file. The folders are new objects:
   * the caller puts them into the node cache or uses the live object that the cache holds.
   * @throws std::runtime_error with the DuckDB message when a statement fails, and when a row
   *         has values that a mapper refuses.
   */
  auto ReadFolderTree() const -> FolderTreeRows;

 private:
  Database& database_;
};

}  // namespace alcedo
