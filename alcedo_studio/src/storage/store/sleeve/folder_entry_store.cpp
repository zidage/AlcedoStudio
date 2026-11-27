//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "storage/store/sleeve/folder_entry_store.hpp"

#include <cstdint>
#include <memory>
#include <vector>

#include "sleeve/sleeve_element/sleeve_element.hpp"
#include "storage/mapper/duckorm/duckdb_expr.hpp"
#include "storage/mapper/sleeve/element/element_mapper.hpp"

namespace alcedo {

FolderEntryStore::FolderEntryStore(Database& database) : database_(database) {}

auto FolderEntryStore::ReadChildFolders(sl_element_id_t folder_id) const
    -> std::vector<std::shared_ptr<SleeveFolder>> {
  duckorm::SqlFragment query{
      "SELECT e.id, e.type, e.element_name, e.added_time, e.modified_time, e.ref_count, "
      "e.mount_id FROM FolderContent fc JOIN Element e ON e.id = fc.element_id "
      "WHERE fc.folder_id = ",
      {}};
  query.append(duckorm::expr::param(static_cast<int64_t>(folder_id)));
  query.append(duckorm::SqlFragment{" AND e.type = ", {}});
  query.append(duckorm::expr::param(static_cast<int64_t>(ElementType::FOLDER)));

  auto guard   = database_.GetConnectionGuard();
  auto db_lock = guard.Lock();
  auto rows    = ElementMapper(guard.conn_).GetElementsByQuery(query);

  std::vector<std::shared_ptr<SleeveFolder>> folders;
  folders.reserve(rows.size());
  for (auto& row : rows) {
    row->SetSyncFlag(SyncFlag::SYNCED);
    folders.push_back(std::static_pointer_cast<SleeveFolder>(std::move(row)));
  }
  return folders;
}

}  // namespace alcedo
