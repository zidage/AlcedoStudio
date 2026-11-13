//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "storage/store/storage_maintenance.hpp"

#include <cstdint>
#include <exception>
#include <format>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "storage/mapper/duckorm/duckdb_expr.hpp"
#include "storage/mapper/duckorm/duckdb_orm.hpp"
#include "storage/mapper/duckorm/duckdb_statement.hpp"
#include "storage/store/duckdb_extension.hpp"
#include "utils/string/convert.hpp"

namespace alcedo {
namespace {

namespace expr = duckorm::expr;

/// The tables of the diagnostics summary and their key columns, in the order of the summary.
struct SummaryTable {
  const char* name;
  const char* key_column;  ///< nullptr when the table has no single key column
};

constexpr SummaryTable kSummaryTables[] = {
    {"Sleeve", "id"},
    {"Image", "id"},
    {"SleeveRoot", "id"},
    {"Element", "id"},
    {"FolderContent", nullptr},
    {"FileImage", "file_id"},
    {"ComboFolder", "combo_id"},
    {"Filter", "combo_id"},
    {"VersionRef", "version_id"},
    {"EditCommit", "commit_hash"},
    {"ImageEditState", "element_id"},
    {"PipelineParam", "file_id"},
    {"SemanticModel", nullptr},
    {"SemanticImageEmbedding", "file_id"},
    {"SemanticImageEmbedding768", "file_id"},
    {"SemanticImageLabel", "file_id"},
    {"SemanticLabelQuery", nullptr},
    {"SemanticLabelPrototype", nullptr},
    {"SemanticLabelPrototype768", nullptr},
};

/// Row count and key range of one summary table (ReadDataSummary).
struct TableSummaryRow {
  int64_t                rows = 0;
  std::optional<int64_t> min_key;
  std::optional<int64_t> max_key;

  using Columns = duckorm::RowColumns<&TableSummaryRow::rows, &TableSummaryRow::min_key,
                                      &TableSummaryRow::max_key>;
};

/// One HNSW index of the database (DropHnswIndexesOfOlderProject).
struct HnswIndexRow {
  std::string schema_name;
  std::string index_name;

  using Columns = duckorm::RowColumns<&HnswIndexRow::schema_name, &HnswIndexRow::index_name>;
};

constexpr std::string_view kHnswIndexCondition =
    "WHERE lower(COALESCE(sql, '')) LIKE '%using hnsw%'";

auto QuoteIdentifier(std::string_view name) -> std::string {
  std::string quoted = "\"";
  for (const char ch : name) {
    if (ch == '"') {
      quoted.push_back('"');
    }
    quoted.push_back(ch);
  }
  quoted.push_back('"');
  return quoted;
}

/// MIN or MAX of @p column as BIGINT. The summary of earlier releases read the cell with
/// duckdb_value_int64, which gives 0 for a text that is not a number and NULL only for a NULL
/// cell; this expression gives the same values.
auto KeyBoundExpression(std::string_view aggregate, std::string_view column) -> std::string {
  const auto bound = std::format("{}({})", aggregate, QuoteIdentifier(column));
  return std::format(
      "CASE WHEN {0} IS NULL THEN NULL ELSE COALESCE(TRY_CAST({0} AS BIGINT), 0) END", bound);
}

/// Run @p sql without binds. Throws std::runtime_error that names @p step.
void RunStep(duckdb_connection& conn, std::string_view sql, std::string_view step) {
  try {
    duckorm::execute(conn, expr::raw(sql));
  } catch (const std::exception& e) {
    throw std::runtime_error(std::format("DuckDB {} failed: {}", step, e.what()));
  }
}

auto HasHnswIndex(duckdb_connection& conn) -> bool {
  try {
    const auto count = duckorm::scalar<int64_t>(
        conn,
        expr::raw(std::format("SELECT COUNT(*) FROM duckdb_indexes() {}", kHnswIndexCondition)));
    return count.value_or(0) > 0;
  } catch (const std::exception& e) {
    throw std::runtime_error(std::format("DuckDB query HNSW indexes failed: {}", e.what()));
  }
}

/// Load the packaged vss extension, which DuckDB needs to bind or drop an existing HNSW index.
void LoadVssExtension(duckdb_connection& conn) {
  std::string load_error;
  if (!LoadPackagedDuckDbExtension(conn, "vss", &load_error)) {
    throw std::runtime_error("DuckDB requires the VSS extension for existing HNSW indexes.\n" +
                             load_error);
  }
}

auto CurrentCatalog(duckdb_connection& conn) -> std::string {
  std::optional<std::string> catalog;
  try {
    catalog = duckorm::scalar<std::string>(conn, expr::raw("SELECT current_catalog();"));
  } catch (const std::exception& e) {
    throw std::runtime_error(std::format("DuckDB query current catalog failed: {}", e.what()));
  }
  if (!catalog.has_value() || catalog->empty()) {
    throw std::runtime_error("DuckDB query current catalog returned no value.");
  }
  return *catalog;
}

}  // namespace

StorageMaintenance::StorageMaintenance(Database& database) : database_(database) {}

auto StorageMaintenance::ReadDataSummary() -> std::vector<StorageTableSummary> {
  auto                             guard = database_.GetConnectionGuard();
  auto                             lock  = guard.Lock();

  std::vector<StorageTableSummary> summary;
  for (const auto& table : kSummaryTables) {
    std::string bounds = "NULL::BIGINT, NULL::BIGINT";
    if (table.key_column != nullptr) {
      bounds = std::format("{}, {}", KeyBoundExpression("MIN", table.key_column),
                           KeyBoundExpression("MAX", table.key_column));
    }
    std::optional<TableSummaryRow> row;
    try {
      row = duckorm::query_one<TableSummaryRow>(
          guard.conn_, expr::raw(std::format("SELECT COUNT(*), {} FROM {}", bounds,
                                             QuoteIdentifier(table.name))));
    } catch (const std::exception&) {
      // The table does not exist in this database (an older project): it has no entry.
      continue;
    }
    if (!row.has_value()) {
      continue;
    }
    summary.push_back(StorageTableSummary{
        .table = table.name, .rows = row->rows, .min_key = row->min_key, .max_key = row->max_key});
  }
  return summary;
}

void StorageMaintenance::DropHnswIndexesOfOlderProject() {
  auto guard = database_.GetConnectionGuard();
  auto lock  = guard.Lock();
  if (!HasHnswIndex(guard.conn_)) {
    return;
  }
  LoadVssExtension(guard.conn_);

  std::vector<HnswIndexRow> indexes;
  try {
    indexes = duckorm::query<HnswIndexRow>(
        guard.conn_,
        expr::raw(std::format("SELECT schema_name, index_name FROM duckdb_indexes() {} "
                              "ORDER BY schema_name, index_name;",
                              kHnswIndexCondition)));
  } catch (const std::exception& e) {
    throw std::runtime_error(std::format("DuckDB query HNSW indexes failed: {}", e.what()));
  }
  for (const auto& index : indexes) {
    RunStep(guard.conn_,
            std::format("DROP INDEX IF EXISTS {}.{};", QuoteIdentifier(index.schema_name),
                        QuoteIdentifier(index.index_name)),
            "drop HNSW index");
  }
}

void StorageMaintenance::WriteSnapshot(const std::filesystem::path& snapshot_path) {
  auto guard = database_.GetConnectionGuard();
  auto lock  = guard.Lock();

  if (HasHnswIndex(guard.conn_)) {
    LoadVssExtension(guard.conn_);
  }
  const auto path_literal   = expr::escape_string(conv::ToBytes(snapshot_path.generic_wstring()));
  const auto source_catalog = QuoteIdentifier(CurrentCatalog(guard.conn_));

  RunStep(guard.conn_, "CHECKPOINT;", "checkpoint");
  RunStep(guard.conn_, std::format("ATTACH '{}' AS pack_snapshot;", path_literal),
          "attach snapshot");
  try {
    RunStep(guard.conn_, std::format("COPY FROM DATABASE {} TO pack_snapshot;", source_catalog),
            "copy database");
    RunStep(guard.conn_, "CHECKPOINT pack_snapshot;", "checkpoint snapshot");
    RunStep(guard.conn_, "DETACH pack_snapshot;", "detach snapshot");
  } catch (...) {
    // The attached snapshot keeps its file open, and its alias would make the ATTACH of every
    // later snapshot on this connection fail. The error of this DETACH is not reported: the
    // rethrown error names the failed step, and a failed DETACH (the snapshot was already
    // detached) changes nothing.
    try {
      duckorm::execute(guard.conn_, expr::raw("DETACH DATABASE IF EXISTS pack_snapshot;"));
    } catch (...) {
    }
    throw;
  }
}

}  // namespace alcedo
