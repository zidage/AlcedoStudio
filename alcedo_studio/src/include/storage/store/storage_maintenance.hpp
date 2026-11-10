//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "storage/store/database.hpp"

namespace alcedo {

/**
 * @brief Row count and key range of one table of the project database.
 *
 * The key range is the MIN and the MAX of the table's key column, read as BIGINT. A text key
 * (VersionRef.version_id, EditCommit.commit_hash) that is not a number reads as 0, the value that
 * the summary of earlier releases stored. Both ends are std::nullopt for a table without a
 * numeric key column and for an empty table.
 */
struct StorageTableSummary {
  std::string            table;
  int64_t                rows = 0;
  std::optional<int64_t> min_key;
  std::optional<int64_t> max_key;
};

/**
 * @brief Operations on the whole project database: the diagnostics summary, the package
 *        snapshot, and the removal of the HNSW indexes of older projects.
 *
 * Storage::Maintenance gives the instance. Each call opens its own connection and holds the
 * database lock for the whole operation, so no store write runs between its statements.
 * Errors are std::runtime_error with the DuckDB message.
 */
class StorageMaintenance {
 public:
  explicit StorageMaintenance(Database& database);

  /**
   * @brief Row count and key range of each project table that the database holds.
   *
   * A diagnostics value, not an integrity check: the project file stores it, and the next open
   * compares it with the database. A table that the database does not have (an older project)
   * is left out. Reads only.
   */
  [[nodiscard]] auto ReadDataSummary() -> std::vector<StorageTableSummary>;

  /**
   * @brief Drop the HNSW indexes that older projects created on the semantic embedding tables.
   *
   * Search scans the folder scope and uses no vector index, and while an HNSW index exists every
   * write to its table needs the vss extension. When an index exists, loads the packaged vss
   * extension (DuckDB cannot bind or drop the index without it) and drops each index.
   *
   * @throws std::runtime_error when the index read, the extension load, or a drop fails. The
   *         indexes dropped before the failure stay dropped.
   */
  void               DropHnswIndexesOfOlderProject();

  /**
   * @brief Copy the live database into a new database file at @p snapshot_path.
   *
   * Runs, in this order: the HNSW index read (and the vss load when one exists), the current
   * catalog read, `CHECKPOINT`, `ATTACH`, `COPY FROM DATABASE`, `CHECKPOINT` of the snapshot,
   * and `DETACH`. The live database is only read.
   *
   * @pre No file exists at @p snapshot_path.
   * @throws std::runtime_error naming the failed step. A failure after the ATTACH detaches the
   *         snapshot first, so a later snapshot can attach the alias again. The caller removes
   *         the snapshot files.
   */
  void               WriteSnapshot(const std::filesystem::path& snapshot_path);

 private:
  Database& database_;
};

}  // namespace alcedo
