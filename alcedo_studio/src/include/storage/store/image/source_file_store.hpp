//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <optional>
#include <span>

#include "storage/store/database.hpp"
#include "type/source_file_record.hpp"
#include "type/type.hpp"

namespace alcedo {

/// Reads and writes source measurements. Each operation takes its own database connection and lock.
class SourceFileStore {
 public:
  explicit SourceFileStore(Database& database) : database_(database) {}

  /// Insert or replace the measurements in one transaction. Each element id must occur once.
  void UpsertSourceFiles(std::span<const SourceFileRecord> records);

  /// Return the source record of a file element, or an empty value if no measurement was stored.
  auto ReadSourceFile(sl_element_id_t element_id) const -> std::optional<SourceFileRecord>;

  void RemoveSourceFiles(std::span<const sl_element_id_t> element_ids);

 private:
  Database& database_;
};

}  // namespace alcedo
