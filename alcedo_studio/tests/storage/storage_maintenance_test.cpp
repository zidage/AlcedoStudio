//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// StorageMaintenance: the diagnostics summary equals the per-table read of earlier releases, a
// failed package snapshot leaves the source database and the connection as they were, and a
// snapshot holds the rows of the source.

#include "storage/store/storage_maintenance.hpp"

#include <duckdb.h>
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "../support/storage_test_access.hpp"
#include "sleeve/storage.hpp"
#include "support/test_temp_directory.hpp"

namespace alcedo {
namespace {

void RunSql(Storage& storage, const std::string& sql) {
  auto          guard = StorageTestAccess::GetDatabase(storage).GetConnectionGuard();
  auto          lock  = guard.Lock();
  duckdb_result result;
  const bool    ok    = duckdb_query(guard.conn_, sql.c_str(), &result) == DuckDBSuccess;
  std::string   error = ok ? "" : duckdb_result_error(&result);
  duckdb_destroy_result(&result);
  ASSERT_TRUE(ok) << sql << ": " << error;
}

/// The value that the summary of earlier releases stored for one query: duckdb_value_int64 of
/// the first cell, std::nullopt for a failed query or a NULL cell.
auto ReadInt64AsEarlierRelease(duckdb_connection conn, const std::string& sql)
    -> std::optional<int64_t> {
  duckdb_result result;
  if (duckdb_query(conn, sql.c_str(), &result) != DuckDBSuccess) {
    duckdb_destroy_result(&result);
    return std::nullopt;
  }
  std::optional<int64_t> value;
  if (duckdb_row_count(&result) > 0 && duckdb_column_count(&result) > 0 &&
      !duckdb_value_is_null(&result, 0, 0)) {
    value = duckdb_value_int64(&result, 0, 0);
  }
  duckdb_destroy_result(&result);
  return value;
}

/// The summary entry of @p table as earlier releases computed it in ProjectService.
auto SummaryAsEarlierRelease(Storage& storage, const std::string& table, const char* key_column)
    -> std::optional<StorageTableSummary> {
  auto       guard = StorageTestAccess::GetDatabase(storage).GetConnectionGuard();
  auto       lock  = guard.Lock();
  const auto rows =
      ReadInt64AsEarlierRelease(guard.conn_, "SELECT COUNT(*) FROM \"" + table + "\"");
  if (!rows.has_value()) {
    return std::nullopt;
  }
  StorageTableSummary summary{.table = table, .rows = *rows};
  if (key_column != nullptr) {
    const std::string key(key_column);
    const auto        min_key = ReadInt64AsEarlierRelease(
        guard.conn_, "SELECT MIN(\"" + key + "\") FROM \"" + table + "\"");
    const auto max_key = ReadInt64AsEarlierRelease(
        guard.conn_, "SELECT MAX(\"" + key + "\") FROM \"" + table + "\"");
    if (min_key.has_value() && max_key.has_value()) {
      summary.min_key = min_key;
      summary.max_key = max_key;
    }
  }
  return summary;
}

struct SummaryTableKey {
  const char* table;
  const char* key_column;
};

// Every table of the summary with its key column (ProjectService before plan phase S6).
constexpr SummaryTableKey kSummaryTables[] = {
    {"Sleeve", "id"},
    {"Image", "id"},
    {"SleeveRoot", "id"},
    {"Element", "id"},
    {"FolderContent", nullptr},
    {"FileImage", "file_id"},
    {"PhotoStack", "stack_id"},
    {"PhotoStackMember", "file_id"},
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

auto FindTable(const std::vector<StorageTableSummary>& summary, const std::string& table)
    -> const StorageTableSummary* {
  for (const auto& entry : summary) {
    if (entry.table == table) {
      return &entry;
    }
  }
  return nullptr;
}

auto CountAttachedSnapshots(Storage& storage) -> int64_t {
  auto guard = StorageTestAccess::GetDatabase(storage).GetConnectionGuard();
  auto lock  = guard.Lock();
  return ReadInt64AsEarlierRelease(
             guard.conn_,
             "SELECT COUNT(*) FROM duckdb_databases() WHERE database_name = 'pack_snapshot'")
      .value_or(-1);
}

class StorageMaintenanceTest : public ::testing::Test {
 protected:
  test::TestTempDirectory temp_dir_;
  void SetUp() override {
    root_    = temp_dir_.Path();
    storage_ = std::make_unique<Storage>(root_ / "project.db");
  }

  void TearDown() override {
    storage_.reset();
  }

  /// Rows in several tables: numeric keys, a text key that is a number and one that is not,
  /// and a table without a key column.
  void InsertRows() {
    RunSql(*storage_, "INSERT INTO Image (id) VALUES (5), (9), (7);");
    RunSql(*storage_, "INSERT INTO Element (id, type) VALUES (11, 0), (3, 1);");
    RunSql(*storage_, "INSERT INTO FolderContent (folder_id, element_id) VALUES (3, 11);");
    RunSql(*storage_,
           "INSERT INTO VersionRef (version_id, element_id, display_name, created_at_unix, "
           "updated_at_unix) VALUES ('42', 11, 'Default', 0, 0), ('v-abc', 11, 'Second', 0, 0);");
  }

  std::filesystem::path    root_;
  std::unique_ptr<Storage> storage_;
};

TEST_F(StorageMaintenanceTest, DataSummaryCountsRowsOfEveryTable) {
  InsertRows();

  const auto summary = storage_->Maintenance().ReadDataSummary();

  ASSERT_EQ(summary.size(), std::size(kSummaryTables));
  for (std::size_t index = 0; index < summary.size(); ++index) {
    const auto& table    = kSummaryTables[index];
    const auto  expected = SummaryAsEarlierRelease(*storage_, table.table, table.key_column);
    ASSERT_TRUE(expected.has_value()) << table.table;
    EXPECT_EQ(summary[index].table, table.table);
    EXPECT_EQ(summary[index].rows, expected->rows) << table.table;
    EXPECT_EQ(summary[index].min_key, expected->min_key) << table.table;
    EXPECT_EQ(summary[index].max_key, expected->max_key) << table.table;
  }

  const auto* image = FindTable(summary, "Image");
  ASSERT_NE(image, nullptr);
  EXPECT_EQ(image->rows, 3);
  EXPECT_EQ(image->min_key, 5);
  EXPECT_EQ(image->max_key, 9);
  // Text keys: '42' reads as 42 and 'v-abc' (the MAX) as 0, as duckdb_value_int64 read them.
  const auto* versions = FindTable(summary, "VersionRef");
  ASSERT_NE(versions, nullptr);
  EXPECT_EQ(versions->rows, 2);
  EXPECT_EQ(versions->min_key, 42);
  EXPECT_EQ(versions->max_key, 0);
  // An empty keyed table and a table without a key column have no key range.
  const auto* commits = FindTable(summary, "EditCommit");
  ASSERT_NE(commits, nullptr);
  EXPECT_EQ(commits->rows, 0);
  EXPECT_FALSE(commits->min_key.has_value());
  const auto* folder_content = FindTable(summary, "FolderContent");
  ASSERT_NE(folder_content, nullptr);
  EXPECT_EQ(folder_content->rows, 1);
  EXPECT_FALSE(folder_content->max_key.has_value());
}

TEST_F(StorageMaintenanceTest, DataSummaryIncludesBothStackTablesWithExactRowCounts) {
  InsertRows();
  RunSql(*storage_, "INSERT INTO Element (id, type) VALUES (12, 0)");
  RunSql(*storage_, "INSERT INTO FileImage (file_id, image_id) VALUES (11, 5), (12, 9)");
  const std::array<sl_element_id_t, 2> ids{11, 12};
  const auto                           stack =
      storage_->GetPhotoStackStore().CreateStack("ec991cd4-a527-4485-b38d-aa8b64aa1c08", ids, 11);
  const auto  summary = storage_->Maintenance().ReadDataSummary();
  const auto* stacks  = FindTable(summary, "PhotoStack");
  const auto* members = FindTable(summary, "PhotoStackMember");
  ASSERT_NE(stacks, nullptr);
  ASSERT_NE(members, nullptr);
  EXPECT_EQ(stacks->rows, 1);
  EXPECT_EQ(stacks->min_key, 0);
  EXPECT_EQ(stacks->max_key, 0);
  EXPECT_EQ(members->rows, 2);
  EXPECT_EQ(members->min_key, 11);
  EXPECT_EQ(members->max_key, 12);
  EXPECT_EQ(storage_->GetPhotoStackStore().ReadMemberIds(stack.stack_id),
            (std::vector<sl_element_id_t>{11, 12}));
}

TEST_F(StorageMaintenanceTest, DataSummaryLeavesOutATableThatTheDatabaseDoesNotHave) {
  RunSql(*storage_, "DROP TABLE SemanticLabelPrototype768;");

  const auto summary = storage_->Maintenance().ReadDataSummary();

  EXPECT_EQ(summary.size(), std::size(kSummaryTables) - 1);
  EXPECT_EQ(FindTable(summary, "SemanticLabelPrototype768"), nullptr);
  EXPECT_NE(FindTable(summary, "SemanticLabelPrototype"), nullptr);
}

TEST_F(StorageMaintenanceTest, SnapshotHoldsTheRowsOfTheSourceDatabase) {
  InsertRows();
  const auto snapshot_path = root_ / "snapshot.db";

  storage_->Maintenance().WriteSnapshot(snapshot_path);

  EXPECT_EQ(CountAttachedSnapshots(*storage_), 0);
  const auto source_summary = storage_->Maintenance().ReadDataSummary();
  Storage    snapshot(snapshot_path);
  const auto snapshot_summary = snapshot.Maintenance().ReadDataSummary();
  ASSERT_EQ(snapshot_summary.size(), source_summary.size());
  for (std::size_t index = 0; index < source_summary.size(); ++index) {
    EXPECT_EQ(snapshot_summary[index].table, source_summary[index].table);
    EXPECT_EQ(snapshot_summary[index].rows, source_summary[index].rows)
        << source_summary[index].table;
    EXPECT_EQ(snapshot_summary[index].min_key, source_summary[index].min_key)
        << source_summary[index].table;
    EXPECT_EQ(snapshot_summary[index].max_key, source_summary[index].max_key)
        << source_summary[index].table;
  }
}

TEST_F(StorageMaintenanceTest, SnapshotFailureKeepsSourceDatabase) {
  InsertRows();
  const auto before        = storage_->Maintenance().ReadDataSummary();

  // A non-empty directory where the snapshot WAL must go: the ATTACH succeeds and the first
  // write of the snapshot fails.
  const auto snapshot_path = root_ / "snapshot_failure.db";
  auto       wal_path      = snapshot_path;
  wal_path += ".wal";
  std::filesystem::create_directories(wal_path);
  std::ofstream(wal_path / "keep") << "x";

  std::string error;
  try {
    storage_->Maintenance().WriteSnapshot(snapshot_path);
  } catch (const std::runtime_error& e) {
    error = e.what();
  }
  EXPECT_NE(error.find("DuckDB "), std::string::npos) << error;
  EXPECT_NE(error.find(" failed: "), std::string::npos) << error;

  // The snapshot is detached, and the source rows are unchanged.
  EXPECT_EQ(CountAttachedSnapshots(*storage_), 0);
  const auto after = storage_->Maintenance().ReadDataSummary();
  ASSERT_EQ(after.size(), before.size());
  for (std::size_t index = 0; index < before.size(); ++index) {
    EXPECT_EQ(after[index].rows, before[index].rows) << before[index].table;
    EXPECT_EQ(after[index].min_key, before[index].min_key) << before[index].table;
    EXPECT_EQ(after[index].max_key, before[index].max_key) << before[index].table;
  }

  // The same connection can attach the alias again.
  std::filesystem::remove_all(wal_path);
  std::filesystem::remove(snapshot_path);
  EXPECT_NO_THROW(storage_->Maintenance().WriteSnapshot(snapshot_path));
  EXPECT_TRUE(std::filesystem::is_regular_file(snapshot_path));
}

TEST_F(StorageMaintenanceTest, HnswDropWithoutAnIndexChangesNothing) {
  InsertRows();
  const auto before = storage_->Maintenance().ReadDataSummary();

  EXPECT_NO_THROW(storage_->Maintenance().DropHnswIndexesOfOlderProject());

  const auto after = storage_->Maintenance().ReadDataSummary();
  ASSERT_EQ(after.size(), before.size());
  for (std::size_t index = 0; index < before.size(); ++index) {
    EXPECT_EQ(after[index].rows, before[index].rows) << before[index].table;
  }
}

}  // namespace
}  // namespace alcedo
