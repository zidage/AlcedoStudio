//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "storage/mapper/sleeve/element/folder_mapper.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <variant>
#include <vector>

#include "storage/mapper/duckorm/duckdb_expr.hpp"
#include "storage/mapper/duckorm/duckdb_statement.hpp"

namespace alcedo {
namespace {

namespace expr = duckorm::expr;

/// `INSERT INTO FolderContent ... VALUES (?, ?), ... ON CONFLICT DO NOTHING` for @p rows rows.
auto MakeInsertFolderContentsSql(size_t rows) -> std::string {
  std::string sql = "INSERT INTO FolderContent (folder_id, element_id) VALUES ";
  for (size_t row = 0; row < rows; ++row) {
    sql += row == 0 ? "(?, ?)" : ", (?, ?)";
  }
  sql += " ON CONFLICT DO NOTHING;";
  return sql;
}

}  // namespace

auto FolderMapper::FromRawData(std::vector<duckorm::VarTypes>&& data) -> FolderMapperParams {
  if (data.size() != FolderMapper::FieldCount()) {
    throw std::runtime_error("Folder Mapper: Invalid DuckFieldDesc for SleeveFolder");
  }

  auto folder_id  = std::get_if<sl_element_id_t>(&data[0]);
  auto element_id = std::get_if<sl_element_id_t>(&data[1]);

  if (folder_id == nullptr || element_id == nullptr) {
    throw std::runtime_error(
        "Folder Mapper: Unmatching types occured when parsing the data from the DB");
  }

  return {*folder_id, *element_id};
}

auto FolderMapper::ToParams(const std::pair<sl_element_id_t, sl_element_id_t> source)
    -> FolderMapperParams {
  return {source.first, source.second};
}

auto FolderMapper::FromParams(FolderMapperParams&& param)
    -> std::pair<sl_element_id_t, sl_element_id_t> {
  return {param.folder_id, param.element_id};
}

auto FolderMapper::GetFolderContent(const sl_element_id_t id) -> std::vector<sl_element_id_t> {
  const auto                   rows = GetWhere(KeyEquals(id));
  std::vector<sl_element_id_t> folder_content;
  folder_content.reserve(rows.size());
  for (const auto& row : rows) {
    folder_content.push_back(row.element_id);
  }
  return folder_content;
}

void FolderMapper::RemoveAllContents(const sl_element_id_t folder_id) { RemoveById(folder_id); }

void FolderMapper::RemoveContentById(const sl_element_id_t content_id) {
  RemoveWhere(expr::column_eq("element_id", static_cast<std::int64_t>(content_id)));
}

void FolderMapper::RemoveContentByIds(std::span<const sl_element_id_t> content_ids) {
  std::vector<sl_element_id_t>        ids;
  std::unordered_set<sl_element_id_t> seen;
  ids.reserve(content_ids.size());
  seen.reserve(content_ids.size() * 2 + 1);
  for (const auto content_id : content_ids) {
    if (content_id != 0 && seen.insert(content_id).second) {
      ids.push_back(content_id);
    }
  }
  if (ids.empty()) {
    return;
  }
  RemoveWhere(expr::in_list(expr::col("element_id"), std::span<const sl_element_id_t>(ids)));
}

void FolderMapper::RemoveFolderContent(sl_element_id_t folder_id, sl_element_id_t content_id) {
  RemoveWhere(expr::and_({KeyEquals(folder_id),
                          expr::column_eq("element_id", static_cast<std::int64_t>(content_id))}));
}

void FolderMapper::InsertFolderContents(sl_element_id_t                  folder_id,
                                        std::span<const sl_element_id_t> content_ids) {
  // Statements of kMultiRowStatementRows rows; full chunks reuse one prepared statement.
  constexpr size_t                  kChunk = duckorm::kMultiRowStatementRows;
  std::optional<duckorm::Statement> full_chunk;
  for (size_t begin = 0; begin < content_ids.size(); begin += kChunk) {
    const size_t                      rows = std::min(kChunk, content_ids.size() - begin);
    std::optional<duckorm::Statement> partial_chunk;
    duckorm::Statement*               insert = nullptr;
    if (rows == kChunk) {
      if (!full_chunk) {
        full_chunk.emplace(conn_, MakeInsertFolderContentsSql(kChunk));
      }
      insert = &*full_chunk;
    } else {
      partial_chunk.emplace(conn_, MakeInsertFolderContentsSql(rows));
      insert = &*partial_chunk;
    }
    idx_t parameter = 1;
    for (size_t row = 0; row < rows; ++row) {
      insert->Bind(parameter++, static_cast<std::int64_t>(folder_id));
      insert->Bind(parameter++, static_cast<std::int64_t>(content_ids[begin + row]));
    }
    (void)insert->Execute();
  }
}

void FolderMapper::RemoveFolderContents(sl_element_id_t                  folder_id,
                                        std::span<const sl_element_id_t> content_ids) {
  if (content_ids.empty()) {
    return;
  }
  RemoveWhere(
      expr::and_({KeyEquals(folder_id), expr::in_list(expr::col("element_id"), content_ids)}));
}

void FolderMapper::UpdateFolderContent(const std::vector<sl_element_id_t>& content,
                                       const sl_element_id_t               folder_id) {
  RemoveAllContents(folder_id);
  for (auto& element_id : content) {
    Insert(std::pair<sl_element_id_t, sl_element_id_t>{folder_id, element_id});
  }
}
}  // namespace alcedo
