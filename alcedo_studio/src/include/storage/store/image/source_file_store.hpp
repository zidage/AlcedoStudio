// Copyright 2026 Yurun Zi
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "storage/store/database.hpp"
#include "type/type.hpp"

namespace alcedo {
/// Minimal query result for import admission, owned by one import request. Contains no Image
/// metadata or edit state; absent byte size identifies an entry made before size recording.
struct SourceFileBinding {
  image_id_t              image_id;
  sl_element_id_t         element_id;
  std::optional<uint64_t> byte_size;
};

/// Stores source byte sizes; full paths remain owned by Image. Each operation acquires a
/// short-lived locked database connection. Throws on storage errors and never opens sources.
class SourceFileStore {
 public:
  explicit SourceFileStore(Database& database) : database_(database) {}
  /// One path/size query, normalized in memory; retains only requested, fully persisted entries.
  auto FindBindings(const std::set<std::wstring>& paths)
      -> std::map<std::wstring, std::vector<SourceFileBinding>>;
  /// Atomically record sizes for live bound Images. Updates only size rows, preserving edits,
  /// ratings, marks, metadata, and memberships. Missing owners reject the complete operation.
  void RecordSizes(std::span<const std::pair<image_id_t, uint64_t>> sizes);
  /// Remove provisional import sizes. The caller owns the ID list.
  void Remove(std::span<const image_id_t> ids);

 private:
  Database& database_;
};
}  // namespace alcedo
