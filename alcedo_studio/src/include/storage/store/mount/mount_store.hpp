//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <ctime>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "storage/mapper/duckorm/duckdb_expr.hpp"
#include "storage/store/database.hpp"
#include "storage/store/image/source_file_store.hpp"
#include "type/mount_records.hpp"
#include "type/source_file_record.hpp"
#include "type/type.hpp"

namespace alcedo {

/**
 * @brief MountRoot and SourceFile rows of one project database (Mounted Folder Plan, MF1).
 *
 * @details A mount is one `MountRoot` row; the elements of a mount hold its id in
 * `Element.mount_id` (ElementStore writes that column with the other element fields). A source
 * record is one `SourceFile` row; ElementStore removes it with its file element.
 *
 * Every public operation requests its own connection from the Database owner and holds the
 * database lock for the whole operation, as ElementStore does. Reads are safe to call from any
 * thread. Each write is one statement or one transaction: a failed write leaves the tables
 * unchanged. SQL failures throw std::runtime_error with the DuckDB message.
 */
class MountStore {
 public:
  MountStore(Database& database, SourceFileStore& sources);

  /**
   * @brief Insert the row of a new mount.
   * @throws std::invalid_argument when the id is 0 or the root path is empty;
   *         std::runtime_error when a mount with the id exists.
   */
  void InsertMountRoot(const MountRootRecord& record);

  /// Every mount, in ascending id order.
  auto ReadMountRoots() const -> std::vector<MountRootRecord>;

  /// The id for the next mount: 1 when no mount is stored, else the largest stored id plus 1.
  auto ReadNextMountId() const -> mount_id_t;

  /**
   * @brief Store a new root directory and volume identity for the mount @p mount_id.
   * @param volume_identity Empty when the volume has no identity (stored as NULL).
   * @throws std::invalid_argument when @p root_path is empty; std::runtime_error when no mount
   *         has the id.
   */
  void UpdateMountRootLocation(mount_id_t mount_id, const std::filesystem::path& root_path,
                               const std::string& volume_identity);

  /**
   * @brief Store the end of the last completed scan of the mount @p mount_id (UTC, seconds).
   * @throws std::runtime_error when no mount has the id.
   */
  void UpdateMountLastScanTime(mount_id_t mount_id, std::time_t last_scan_at);

  /**
   * @brief Store the display name of the volume of the mount @p mount_id (#1076).
   * @param volume_name Empty when the volume has no name (stored as NULL).
   * @throws std::runtime_error when no mount has the id.
   */
  void UpdateMountVolumeName(mount_id_t mount_id, const std::string& volume_name);

  /**
   * @brief Remove the row of the mount @p mount_id. Elements, links, and source records stay.
   * @throws std::runtime_error when no mount has the id.
   */
  void RemoveMountRoot(mount_id_t mount_id);

  /**
   * @brief Insert or replace the source record of each element of @p records, in one
   *        transaction.
   * @throws std::invalid_argument when two records name one element, or a record is invalid
   *         (see SourceFileMapper::ToParams); nothing is written then.
   */
  void UpsertSourceFiles(std::span<const SourceFileRecord> records);

  /// The source record of @p element_id; empty when the element has none.
  auto ReadSourceFile(sl_element_id_t element_id) const -> std::optional<SourceFileRecord>;

  /// The source records of the elements whose `Element.mount_id` is @p mount_id, in ascending
  /// element id order.
  auto ReadSourceFilesOfMount(mount_id_t mount_id) const -> std::vector<SourceFileRecord>;

  /**
   * @brief Set the state of the source records of @p element_ids, in one transaction.
   *
   * @param missing_since The time when the files became missing (UTC, seconds). Required for
   *        SourceFileState::kMissing and empty for SourceFileState::kPresent; a present record
   *        stores NULL.
   * @throws std::invalid_argument when @p missing_since does not agree with @p state;
   *         std::runtime_error when an element has no source record. Nothing is written then.
   */
  void UpdateSourceFileState(std::span<const sl_element_id_t> element_ids, SourceFileState state,
                             std::optional<std::time_t> missing_since);

  /// Remove the source records of @p element_ids. An element without a record is skipped.
  void RemoveSourceFiles(std::span<const sl_element_id_t> element_ids);

  /// The location of each file element whose `Element.mount_id` is @p mount_id, in ascending
  /// element id order. A file without an image binding or Image row is absent.
  auto ReadFileLocationsOfMount(mount_id_t mount_id) const -> std::vector<FileLocationRecord>;

  /// The location of each ordinary file element (`Element.mount_id` is NULL), in ascending
  /// element id order. The mount scan takes the adoption candidates from it.
  auto ReadOrdinaryFileLocations() const -> std::vector<FileLocationRecord>;

 private:
  /// The location of each file element that @p condition (on `Element e`) selects.
  auto ReadFileLocations(const duckorm::SqlFragment& condition) const
      -> std::vector<FileLocationRecord>;

  Database& database_;
  SourceFileStore& sources_;
};

}  // namespace alcedo
