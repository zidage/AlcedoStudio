//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "storage/store/image/source_file_store.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>
#include <vector>

#include "storage/mapper/duckorm/duckdb_orm.hpp"
#include "storage/mapper/image/source_file_mapper.hpp"

namespace alcedo {

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
  SourceFileMapper(guard.conn_).UpsertParamsRows(rows);
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
  SourceFileMapper(guard.conn_).RemoveByIds(element_ids);
}

}  // namespace alcedo
