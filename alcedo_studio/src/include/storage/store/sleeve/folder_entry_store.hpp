//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "sleeve/sleeve_element/sleeve_folder.hpp"
#include "storage/mapper/duckorm/duckdb_expr.hpp"
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

/// One stored link of a file into a mounted folder (FolderEntryStore::ReadMountFileFolders).
struct MountFileFolderRow {
  sl_element_id_t           file_id                                             = 0;
  sl_element_id_t           folder_id                                           = 0;
  /// The stored `Element.mount_id` of the file: empty for an ordinary file.
  std::optional<mount_id_t> file_mount_id                                       = std::nullopt;

  auto                      operator==(const MountFileFolderRow&) const -> bool = default;
};

/**
 * @brief Reads of the folder structure (`Element` joined with `FolderContent`) that load no
 *        file element (Sleeve Node Cache Plan, SN1, SN3, SN4, and SN5).
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

  /**
   * @brief The ids of the file elements that have a `FolderContent` row in @p folder_id, in id
   *        order, with one statement (Sleeve Node Cache Plan, SN4). Loads no element.
   * @throws std::runtime_error with the DuckDB message when the statement fails.
   */
  auto ReadFolderFiles(sl_element_id_t folder_id) const -> std::vector<sl_element_id_t>;

  /// Stored child folder ids in entry order. Loads no elements.
  auto ReadChildFolderIds(sl_element_id_t folder_id) const -> std::vector<sl_element_id_t>;

  /**
   * @brief The ids of @p file_ids that name a file element with a `FolderContent` row in
   *        @p folder_id, in id order, with one statement. Loads no element. An empty
   *        @p file_ids gives an empty result and sends no statement.
   * @throws std::runtime_error with the DuckDB message when the statement fails.
   */
  auto ReadHeldFiles(sl_element_id_t folder_id, std::span<const sl_element_id_t> file_ids) const
      -> std::vector<sl_element_id_t>;

  /**
   * @brief Every stored link of a file element into a folder element with the mount id
   *        @p mount_id, ordered by file id and then by folder id, with one statement (Sleeve
   *        Node Cache Plan, SN5). Loads no element.
   *
   * @details Each row carries the stored mount id of the file, so the caller can decide a file
   * that the node cache holds from memory and another file from its row. A file of another mount
   * or an ordinary file that a mounted folder holds is in the result too.
   * @throws std::runtime_error with the DuckDB message when the statement fails.
   */
  auto ReadMountFileFolders(mount_id_t mount_id) const -> std::vector<MountFileFolderRow>;

 private:
  /// The child ids of @p type under @p folder_id that @p condition also selects (an extra
  /// predicate on `fc.element_id`, or an empty fragment).
  auto ReadChildIds(sl_element_id_t folder_id, ElementType type, duckorm::SqlFragment condition) const
      -> std::vector<sl_element_id_t>;

  Database& database_;
};

}  // namespace alcedo
