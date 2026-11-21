//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "storage/store/mount/mount_store.hpp"

#include <algorithm>
#include <cstdint>
#include <format>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "storage/mapper/duckorm/duckdb_expr.hpp"
#include "storage/mapper/duckorm/duckdb_orm.hpp"
#include "storage/mapper/mount/mount_root_mapper.hpp"
#include "storage/mapper/mount/source_file_mapper.hpp"
#include "storage/mapper/utc_timestamp_text.hpp"
#include "utils/string/convert.hpp"

namespace alcedo {
namespace {
namespace expr = duckorm::expr;

constexpr const char* kMountRootColumns =
    "SELECT id, root_folder_id, volume_identity, root_path, last_scan_at FROM MountRoot ";
constexpr const char* kSourceFileColumns =
    "SELECT s.element_id, s.byte_size, s.modified_ns, s.fingerprint, s.fingerprint_version, "
    "s.state, s.missing_since FROM SourceFile s ";

/// `?` bound to @p text, or `NULL` when @p text is empty.
auto NullableTextParam(const std::string& text) -> duckorm::SqlFragment {
  return text.empty() ? expr::lit_null() : expr::param(text);
}

/// Throw unless the statement changed one MountRoot row.
void RequireOneMountChanged(idx_t changed_rows, mount_id_t mount_id, const char* operation) {
  if (changed_rows != 1) {
    throw std::runtime_error(
        std::format("MountStore: {}: no mount has the id {}", operation, mount_id));
  }
}

auto SortedUniqueIds(std::span<const sl_element_id_t> ids) -> std::vector<sl_element_id_t> {
  std::vector<sl_element_id_t> sorted(ids.begin(), ids.end());
  std::sort(sorted.begin(), sorted.end());
  sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
  return sorted;
}
}  // namespace

MountStore::MountStore(Database& database) : database_(database) {}

void MountStore::InsertMountRoot(const MountRootRecord& record) {
  if (record.id == 0) {
    throw std::invalid_argument("MountStore: a mount id starts at 1");
  }
  if (record.root_path.empty()) {
    throw std::invalid_argument(
        std::format("MountStore: the mount {} has no root directory", record.id));
  }
  auto guard   = database_.GetConnectionGuard();
  auto db_lock = guard.Lock();
  MountRootMapper(guard.conn_).Insert(record);
}

auto MountStore::ReadMountRoots() const -> std::vector<MountRootRecord> {
  auto guard   = database_.GetConnectionGuard();
  auto db_lock = guard.Lock();
  return MountRootMapper(guard.conn_)
      .GetByQuery(duckorm::SqlFragment{std::string(kMountRootColumns) + "ORDER BY id", {}});
}

auto MountStore::ReadNextMountId() const -> mount_id_t {
  auto       guard   = database_.GetConnectionGuard();
  auto       db_lock = guard.Lock();
  const auto largest = duckorm::scalar<int64_t>(
      guard.conn_, expr::raw("SELECT CAST(COALESCE(MAX(id), 0) AS BIGINT) FROM MountRoot"));
  const int64_t next = largest.value_or(0) + 1;
  if (next < 1 || next > std::numeric_limits<mount_id_t>::max()) {
    throw std::runtime_error(
        std::format("MountStore: the next mount id {} is outside the mount id range", next));
  }
  return static_cast<mount_id_t>(next);
}

void MountStore::UpdateMountRootLocation(mount_id_t                   mount_id,
                                         const std::filesystem::path& root_path,
                                         const std::string&           volume_identity) {
  if (root_path.empty()) {
    throw std::invalid_argument(
        std::format("MountStore: the mount {} cannot move to an empty root directory", mount_id));
  }
  auto statement = expr::raw("UPDATE MountRoot SET root_path = ");
  statement.append(expr::param(conv::ToBytes(root_path.wstring())));
  statement.append(expr::raw(", volume_identity = "));
  statement.append(NullableTextParam(volume_identity));
  statement.append(expr::raw(" WHERE id = "));
  statement.append(expr::param(static_cast<int64_t>(mount_id)));

  auto guard   = database_.GetConnectionGuard();
  auto db_lock = guard.Lock();
  RequireOneMountChanged(duckorm::execute_count(guard.conn_, statement), mount_id,
                         "UpdateMountRootLocation");
}

void MountStore::UpdateMountLastScanTime(mount_id_t mount_id, std::time_t last_scan_at) {
  auto statement = expr::raw("UPDATE MountRoot SET last_scan_at = CAST(");
  statement.append(expr::param(FormatUtcTimestampText(last_scan_at)));
  statement.append(expr::raw(" AS TIMESTAMP) WHERE id = "));
  statement.append(expr::param(static_cast<int64_t>(mount_id)));

  auto guard   = database_.GetConnectionGuard();
  auto db_lock = guard.Lock();
  RequireOneMountChanged(duckorm::execute_count(guard.conn_, statement), mount_id,
                         "UpdateMountLastScanTime");
}

void MountStore::RemoveMountRoot(mount_id_t mount_id) {
  auto statement = expr::raw("DELETE FROM MountRoot WHERE id = ");
  statement.append(expr::param(static_cast<int64_t>(mount_id)));

  auto guard   = database_.GetConnectionGuard();
  auto db_lock = guard.Lock();
  RequireOneMountChanged(duckorm::execute_count(guard.conn_, statement), mount_id,
                         "RemoveMountRoot");
}

void MountStore::UpsertSourceFiles(std::span<const SourceFileRecord> records) {
  if (records.empty()) {
    return;
  }
  std::vector<SourceFileMapperParams> rows;
  std::vector<sl_element_id_t>        element_ids;
  rows.reserve(records.size());
  element_ids.reserve(records.size());
  for (const auto& record : records) {
    rows.push_back(SourceFileMapper::ToParams(record));
    element_ids.push_back(record.element_id);
  }
  if (SortedUniqueIds(element_ids).size() != element_ids.size()) {
    throw std::invalid_argument("MountStore: two source records name one element");
  }

  auto                 guard   = database_.GetConnectionGuard();
  auto                 db_lock = guard.Lock();
  duckorm::Transaction transaction(guard.conn_);
  SourceFileMapper(guard.conn_).UpsertParamsRows(rows);
  transaction.commit();
}

auto MountStore::ReadSourceFile(sl_element_id_t element_id) const
    -> std::optional<SourceFileRecord> {
  auto guard   = database_.GetConnectionGuard();
  auto db_lock = guard.Lock();
  auto row     = SourceFileMapper(guard.conn_).GetById(element_id);
  if (!row.has_value()) {
    return std::nullopt;
  }
  return SourceFileMapper::FromParams(std::move(*row));
}

auto MountStore::ReadSourceFilesOfMount(mount_id_t mount_id) const
    -> std::vector<SourceFileRecord> {
  auto query = expr::raw(std::string(kSourceFileColumns) +
                         "JOIN Element e ON e.id = s.element_id WHERE e.mount_id = ");
  query.append(expr::param(static_cast<int64_t>(mount_id)));
  query.append(expr::raw(" ORDER BY s.element_id"));

  auto guard   = database_.GetConnectionGuard();
  auto db_lock = guard.Lock();
  return SourceFileMapper(guard.conn_).GetByQuery(query);
}

void MountStore::UpdateSourceFileState(std::span<const sl_element_id_t> element_ids,
                                       SourceFileState                  state,
                                       std::optional<std::time_t>       missing_since) {
  if ((state == SourceFileState::kMissing) != missing_since.has_value()) {
    throw std::invalid_argument(
        "MountStore: a missing source record needs its missing time, and a present record has "
        "none");
  }
  const auto ids = SortedUniqueIds(element_ids);
  if (ids.empty()) {
    return;
  }
  auto statement = expr::raw("UPDATE SourceFile SET state = ");
  statement.append(expr::param(static_cast<int64_t>(state)));
  statement.append(expr::raw(", missing_since = "));
  if (missing_since.has_value()) {
    statement.append(expr::raw("CAST("));
    statement.append(expr::param(FormatUtcTimestampText(*missing_since)));
    statement.append(expr::raw(" AS TIMESTAMP)"));
  } else {
    statement.append(expr::raw("NULL"));
  }
  statement.append(expr::raw(" WHERE "));
  statement.append(expr::in_list(expr::col("element_id"), std::span<const sl_element_id_t>(ids)));

  auto                 guard   = database_.GetConnectionGuard();
  auto                 db_lock = guard.Lock();
  duckorm::Transaction transaction(guard.conn_);
  const auto           changed_rows = duckorm::execute_count(guard.conn_, statement);
  if (changed_rows != ids.size()) {
    throw std::runtime_error(std::format("MountStore: {} of {} elements have no source record",
                                         ids.size() - changed_rows, ids.size()));
  }
  transaction.commit();
}

void MountStore::RemoveSourceFiles(std::span<const sl_element_id_t> element_ids) {
  if (element_ids.empty()) {
    return;
  }
  auto guard   = database_.GetConnectionGuard();
  auto db_lock = guard.Lock();
  SourceFileMapper(guard.conn_).RemoveByIds(element_ids);
}

}  // namespace alcedo
