//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "storage/store/sleeve/folder_entry_store.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include "sleeve/sleeve_element/sleeve_element.hpp"
#include "storage/mapper/duckorm/duckdb_expr.hpp"
#include "storage/mapper/sleeve/element/element_mapper.hpp"
#include "storage/mapper/sleeve/element/folder_mapper.hpp"

namespace alcedo {

FolderEntryStore::FolderEntryStore(Database& database) : database_(database) {}

auto FolderEntryStore::ReadFolderTree() const -> FolderTreeRows {
  const auto folder_type = duckorm::expr::param(static_cast<int64_t>(ElementType::FOLDER));

  duckorm::SqlFragment folder_query{
      "SELECT id, type, element_name, added_time, modified_time, ref_count, mount_id "
      "FROM Element WHERE type = ",
      {}};
  folder_query.append(folder_type);
  folder_query.append(duckorm::SqlFragment{" ORDER BY id", {}});

  duckorm::SqlFragment edge_query{
      "SELECT fc.folder_id, fc.element_id FROM FolderContent fc "
      "JOIN Element parent ON parent.id = fc.folder_id "
      "JOIN Element child ON child.id = fc.element_id WHERE parent.type = ",
      {}};
  edge_query.append(folder_type);
  edge_query.append(duckorm::SqlFragment{" AND child.type = ", {}});
  edge_query.append(folder_type);
  edge_query.append(duckorm::SqlFragment{" ORDER BY fc.folder_id, fc.element_id", {}});

  auto           guard   = database_.GetConnectionGuard();
  auto           db_lock = guard.Lock();
  auto           rows    = ElementMapper(guard.conn_).GetElementsByQuery(folder_query);

  FolderTreeRows tree;
  tree.edges_ = FolderMapper(guard.conn_).GetByQuery(edge_query);
  tree.folders_.reserve(rows.size());
  for (auto& row : rows) {
    row->SetSyncFlag(SyncFlag::SYNCED);
    tree.folders_.push_back(std::static_pointer_cast<SleeveFolder>(std::move(row)));
  }
  return tree;
}

auto FolderEntryStore::ReadFolderFiles(sl_element_id_t folder_id) const
    -> std::vector<sl_element_id_t> {
  return ReadFileRows(folder_id, duckorm::SqlFragment{});
}

auto FolderEntryStore::ReadHeldFiles(sl_element_id_t                  folder_id,
                                     std::span<const sl_element_id_t> file_ids) const
    -> std::vector<sl_element_id_t> {
  if (file_ids.empty()) {
    return {};
  }
  auto condition = duckorm::SqlFragment{" AND ", {}};
  condition.append(duckorm::expr::in_list(duckorm::expr::col("fc.element_id"), file_ids));
  return ReadFileRows(folder_id, std::move(condition));
}

auto FolderEntryStore::ReadFileRows(sl_element_id_t folder_id, duckorm::SqlFragment condition) const
    -> std::vector<sl_element_id_t> {
  duckorm::SqlFragment query{
      "SELECT fc.folder_id, fc.element_id FROM FolderContent fc "
      "JOIN Element e ON e.id = fc.element_id WHERE fc.folder_id = ",
      {}};
  query.append(duckorm::expr::param(static_cast<int64_t>(folder_id)));
  query.append(duckorm::SqlFragment{" AND e.type = ", {}});
  query.append(duckorm::expr::param(static_cast<int64_t>(ElementType::FILE)));
  query.append(std::move(condition));
  query.append(duckorm::SqlFragment{" ORDER BY fc.element_id", {}});

  auto                         guard   = database_.GetConnectionGuard();
  auto                         db_lock = guard.Lock();
  const auto                   rows    = FolderMapper(guard.conn_).GetByQuery(query);
  std::vector<sl_element_id_t> file_ids;
  file_ids.reserve(rows.size());
  for (const auto& [_, file_id] : rows) {
    file_ids.push_back(file_id);
  }
  return file_ids;
}

}  // namespace alcedo
