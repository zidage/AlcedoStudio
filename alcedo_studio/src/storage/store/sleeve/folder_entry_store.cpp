//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "storage/store/sleeve/folder_entry_store.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "sleeve/sleeve_element/sleeve_element.hpp"
#include "storage/mapper/duckorm/duckdb_expr.hpp"
#include "storage/mapper/duckorm/duckdb_orm.hpp"
#include "storage/mapper/sleeve/element/element_mapper.hpp"
#include "storage/mapper/sleeve/element/folder_mapper.hpp"

namespace alcedo {
namespace {

/// One row of FolderEntryStore::ReadMountFileFolders. `Element.mount_id` is NULL for an
/// ordinary file.
struct MountFileFolderQueryRow {
  int64_t                file_id   = 0;
  int64_t                folder_id = 0;
  std::optional<int64_t> file_mount_id;
  using Columns =
      duckorm::RowColumns<&MountFileFolderQueryRow::file_id, &MountFileFolderQueryRow::folder_id,
                          &MountFileFolderQueryRow::file_mount_id>;
};

}  // namespace

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
  return ReadChildIds(folder_id, ElementType::FILE, duckorm::SqlFragment{});
}

auto FolderEntryStore::ReadHeldFiles(sl_element_id_t                  folder_id,
                                     std::span<const sl_element_id_t> file_ids) const
    -> std::vector<sl_element_id_t> {
  if (file_ids.empty()) {
    return {};
  }
  auto condition = duckorm::SqlFragment{" AND ", {}};
  condition.append(duckorm::expr::in_list(duckorm::expr::col("fc.element_id"), file_ids));
  return ReadChildIds(folder_id, ElementType::FILE, std::move(condition));
}

auto FolderEntryStore::ReadMountFileFolders(mount_id_t mount_id) const
    -> std::vector<MountFileFolderRow> {
  duckorm::SqlFragment query{
      "SELECT fc.element_id, fc.folder_id, fe.mount_id FROM FolderContent fc "
      "JOIN Element mf ON mf.id = fc.folder_id "
      "JOIN Element fe ON fe.id = fc.element_id WHERE mf.type = ",
      {}};
  query.append(duckorm::expr::param(static_cast<int64_t>(ElementType::FOLDER)));
  query.append(duckorm::SqlFragment{" AND mf.mount_id = ", {}});
  query.append(duckorm::expr::param(static_cast<int64_t>(mount_id)));
  query.append(duckorm::SqlFragment{" AND fe.type = ", {}});
  query.append(duckorm::expr::param(static_cast<int64_t>(ElementType::FILE)));
  query.append(duckorm::SqlFragment{" ORDER BY fc.element_id, fc.folder_id", {}});

  std::vector<MountFileFolderRow> links;
  auto                            guard   = database_.GetConnectionGuard();
  auto                            db_lock = guard.Lock();
  for (const auto& row : duckorm::query<MountFileFolderQueryRow>(guard.conn_, query)) {
    links.push_back(MountFileFolderRow{
        .file_id   = static_cast<sl_element_id_t>(row.file_id),
        .folder_id = static_cast<sl_element_id_t>(row.folder_id),
        .file_mount_id =
            row.file_mount_id.has_value()
                ? std::optional<mount_id_t>(static_cast<mount_id_t>(*row.file_mount_id))
                : std::nullopt,
    });
  }
  return links;
}

auto FolderEntryStore::ReadChildFolderIds(sl_element_id_t folder_id) const
    -> std::vector<sl_element_id_t> {
  return ReadChildIds(folder_id, ElementType::FOLDER, {});
}

auto FolderEntryStore::ReadChildIds(sl_element_id_t folder_id, ElementType type,
                                    duckorm::SqlFragment condition) const
    -> std::vector<sl_element_id_t> {
  duckorm::SqlFragment query{
      "SELECT fc.folder_id, fc.element_id FROM FolderContent fc "
      "JOIN Element e ON e.id = fc.element_id WHERE fc.folder_id = ",
      {}};
  query.append(duckorm::expr::param(static_cast<int64_t>(folder_id)));
  query.append(duckorm::SqlFragment{" AND e.type = ", {}});
  query.append(duckorm::expr::param(static_cast<int64_t>(type)));
  query.append(std::move(condition));
  query.append(duckorm::SqlFragment{" ORDER BY fc.element_id", {}});

  auto                         guard   = database_.GetConnectionGuard();
  auto                         db_lock = guard.Lock();
  const auto                   rows    = FolderMapper(guard.conn_).GetByQuery(query);
  std::vector<sl_element_id_t> child_ids;
  child_ids.reserve(rows.size());
  for (const auto& [_, child_id] : rows) {
    child_ids.push_back(child_id);
  }
  return child_ids;
}

}  // namespace alcedo
