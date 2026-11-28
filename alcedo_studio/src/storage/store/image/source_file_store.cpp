//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "storage/store/image/source_file_store.hpp"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

#include "storage/mapper/duckorm/duckdb_orm.hpp"
#include "storage/mapper/image/source_file_mapper.hpp"

namespace alcedo {
namespace {

struct ChangedEntryRow {
  int64_t id;
  using Columns = duckorm::RowColumns<&ChangedEntryRow::id>;
};

}  // namespace

void SourceFileStore::UpsertSourceFiles(std::span<const SourceFileRecord> records) {
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
  std::sort(element_ids.begin(), element_ids.end());
  if (std::adjacent_find(element_ids.begin(), element_ids.end()) != element_ids.end()) {
    throw std::invalid_argument("SourceFileStore: two source records name one element");
  }

  auto                 guard   = database_.GetConnectionGuard();
  auto                 db_lock = guard.Lock();
  duckorm::Transaction transaction(guard.conn_);
  SourceFileMapper     mapper(guard.conn_);
  mapper.ClearChangeMarks(element_ids);
  mapper.UpsertParamsRows(rows);
  transaction.commit();
}

auto SourceFileStore::ReadSourceFile(sl_element_id_t element_id) const
    -> std::optional<SourceFileRecord> {
  auto guard   = database_.GetConnectionGuard();
  auto db_lock = guard.Lock();
  auto row     = SourceFileMapper(guard.conn_).GetById(element_id);
  if (!row.has_value()) {
    return std::nullopt;
  }
  return SourceFileMapper::FromParams(std::move(*row));
}

void SourceFileStore::RemoveSourceFiles(std::span<const sl_element_id_t> element_ids) {
  if (element_ids.empty()) {
    return;
  }
  auto guard   = database_.GetConnectionGuard();
  auto db_lock = guard.Lock();
  duckorm::Transaction transaction(guard.conn_);
  SourceFileMapper(guard.conn_).RemoveByIds(element_ids);
  transaction.commit();
}

auto SourceFileStore::ReadChangedEntryIds() const -> std::vector<sl_element_id_t> {
  auto       guard = database_.GetConnectionGuard();
  auto       lock  = guard.Lock();
  const auto rows  = duckorm::query<ChangedEntryRow>(
      guard.conn_,
      duckorm::expr::raw(
          "SELECT c.element_id FROM SourceFileChanged c JOIN Element e ON e.id=c.element_id "
          "JOIN SourceFile s ON s.element_id=c.element_id WHERE e.type=0 AND e.mount_id IS NULL "
          "ORDER BY c.element_id"));
  std::vector<sl_element_id_t> ids;
  ids.reserve(rows.size());
  for (const auto& row : rows) ids.push_back(static_cast<sl_element_id_t>(row.id));
  return ids;
}

auto SourceFileStore::UpdateChangeMarks(std::span<const SourceChangeMark> changes) -> bool {
  if (changes.empty()) return false;
  namespace expr = duckorm::expr;
  auto values    = expr::raw("(VALUES ");
  for (const auto& change : changes) {
    if (&change != changes.data()) values.append(expr::raw(","));
    values.append(expr::raw("("));
    values.append(expr::param(static_cast<int64_t>(change.element_id)));
    values.append(expr::raw(","));
    values.append(expr::param(change.baseline.hash.ToString()));
    values.append(expr::raw(","));
    values.append(expr::param(static_cast<int64_t>(change.baseline.version)));
    values.append(expr::raw(change.changed ? ", TRUE)" : ", FALSE)"));
  }
  values.append(
      expr::raw(") AS v(id, fingerprint, version, changed) "
                "JOIN Element e ON e.id=v.id JOIN SourceFile s ON s.element_id=v.id "
                "WHERE e.type=0 AND e.mount_id IS NULL AND s.fingerprint=v.fingerprint "
                "AND s.fingerprint_version=v.version"));

  auto add = expr::raw("INSERT INTO SourceFileChanged SELECT v.id FROM ");
  add.append(values);
  add.append(expr::raw(" AND v.changed ON CONFLICT DO NOTHING"));
  auto remove = expr::raw("DELETE FROM SourceFileChanged WHERE element_id IN (SELECT v.id FROM ");
  remove.append(values);
  remove.append(expr::raw(" AND NOT v.changed)"));

  auto                 guard = database_.GetConnectionGuard();
  auto                 lock  = guard.Lock();
  duckorm::Transaction transaction(guard.conn_);
  const auto           inserted = duckorm::execute_count(guard.conn_, add);
  const auto           removed  = duckorm::execute_count(guard.conn_, remove);
  transaction.commit();
  return inserted != 0 || removed != 0;
}

}  // namespace alcedo
