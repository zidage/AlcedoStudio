//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <memory>
#include <vector>

#include "sleeve/sleeve_element/sleeve_folder.hpp"
#include "storage/store/database.hpp"
#include "type/type.hpp"

namespace alcedo {

/**
 * @brief Reads of the folder structure (`Element` joined with `FolderContent`) that load no
 *        file element (Sleeve Node Cache Plan, SN1).
 *
 * @details The store has no state. Each operation takes the connection and the database lock of
 * the Database owner for the whole operation, so it is safe to call from any thread. A
 * `FolderContent` row whose element has no `Element` row is not in a result.
 */
class FolderEntryStore {
 public:
  explicit FolderEntryStore(Database& database);

  /**
   * @brief The child folders of @p folder_id, read with one statement, with sync flag SYNCED.
   *
   * @details The folders are new objects: the caller puts them into the node map or uses the
   * live object that the map already holds. Their children are not loaded. Order is unspecified.
   * @throws std::runtime_error with the DuckDB message when the statement fails, and when a row
   *         has values that ElementMapper refuses.
   */
  auto ReadChildFolders(sl_element_id_t folder_id) const
      -> std::vector<std::shared_ptr<SleeveFolder>>;

 private:
  Database& database_;
};

}  // namespace alcedo
