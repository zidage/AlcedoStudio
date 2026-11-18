// Copyright 2026 Yurun Zi
// SPDX-License-Identifier: GPL-3.0-only
#include "storage/store/image/source_file_store.hpp"

#include <array>
#include <stdexcept>
#include <variant>

#include "storage/mapper/duckorm/duckdb_orm.hpp"
#include "utils/file/source_image_path.hpp"
#include "utils/string/convert.hpp"

namespace alcedo {
namespace expr = duckorm::expr;

auto SourceFileStore::FindBindings(const std::set<std::wstring>& paths)
    -> std::map<std::wstring, std::vector<SourceFileBinding>> {
  std::map<std::wstring, std::vector<SourceFileBinding>> matches;
  if (paths.empty()) return matches;
  auto                                        guard = database_.GetConnectionGuard();
  auto                                        lock  = guard.Lock();
  const std::array<duckorm::DuckFieldDesc, 5> fields{
      {{"id", duckorm::DuckDBType::INT64, 0},
       {"image_path", duckorm::DuckDBType::STRING, 0},
       {"file_id", duckorm::DuckDBType::INT64, 0},
       {"byte_size", duckorm::DuckDBType::UINT64, 0},
       {"source_id", duckorm::DuckDBType::NULLABLE_INT64, 0}}};
  const auto rows = duckorm::select_by_query(
      guard.conn_, fields, fields.size(),
      "SELECT DISTINCT i.id, i.image_path, f.file_id, s.byte_size, s.image_id "
      "FROM Image i JOIN FileImage f ON f.image_id = i.id "
      "JOIN PipelineRoot r ON r.element_id = f.file_id "
      "LEFT JOIN ImageSourceSize s ON s.image_id = i.id");
  for (const auto& row : rows) {
    const auto path = image_path_t(conv::FromBytes(duckorm::cell_text(row[1])));
    if (!path.is_absolute()) continue;
    auto normalized = NormalizeSourceImagePath(path);
    if (!paths.contains(normalized)) continue;
    const auto size = std::get<std::optional<int64_t>>(row[4])
                          ? std::optional<uint64_t>(std::get<uint64_t>(row[3]))
                          : std::nullopt;
    matches[normalized].push_back({static_cast<image_id_t>(std::get<int64_t>(row[0])),
                                   static_cast<sl_element_id_t>(std::get<int64_t>(row[2])), size});
  }
  return matches;
}

void SourceFileStore::RecordSizes(std::span<const std::pair<image_id_t, uint64_t>> sizes) {
  if (sizes.empty()) return;
  std::string ids;
  std::string values;
  for (const auto& [id, size] : sizes) {
    if (!ids.empty()) {
      ids += ',';
      values += ',';
    }
    ids += std::to_string(id);
    values += '(' + std::to_string(id) + ',' + std::to_string(size) + ')';
  }
  auto                 guard = database_.GetConnectionGuard();
  auto                 lock  = guard.Lock();
  duckorm::Transaction transaction(guard.conn_);
  const auto           count = duckorm::select_int64(
      guard.conn_, expr::raw("SELECT count(DISTINCT i.id) FROM Image i "
                             "JOIN FileImage f ON f.image_id = i.id WHERE i.id IN (" +
                             ids + ')'));
  if (count.value_or(0) != static_cast<int64_t>(sizes.size())) {
    throw std::runtime_error("Source size requires distinct live image bindings");
  }
  duckorm::execute(guard.conn_, expr::raw("INSERT INTO ImageSourceSize VALUES " + values +
                                          " ON CONFLICT(image_id) DO UPDATE "
                                          "SET byte_size = excluded.byte_size"));
  transaction.commit();
}

void SourceFileStore::Remove(std::span<const image_id_t> ids) {
  if (ids.empty()) return;
  std::string list;
  for (auto id : ids) {
    if (!list.empty()) list += ',';
    list += std::to_string(id);
  }
  auto guard = database_.GetConnectionGuard();
  auto lock  = guard.Lock();
  duckorm::execute(guard.conn_,
                   expr::raw("DELETE FROM ImageSourceSize WHERE image_id IN (" + list + ')'));
}
}  // namespace alcedo
