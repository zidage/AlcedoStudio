//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "storage/mapper/duckorm/duckdb_types.hpp"
#include "storage/mapper/mapper.hpp"
#include "type/mount_records.hpp"
#include "type/type.hpp"

namespace alcedo {
/// One MountRoot row (see `mount_schema_query` in database.hpp), in table order.
struct MountRootMapperParams {
  mount_id_t      id             = 0;
  sl_element_id_t root_folder_id = 0;
  /// Empty writes NULL.
  std::string     volume_identity;
  /// UTF-8 text of the absolute root directory.
  std::string     root_path;
  /// `YYYY-MM-DD HH:MM:SS` UTC; empty writes NULL.
  std::string     last_scan_at;
  /// Empty writes NULL.
  std::string     volume_name;
};

/**
 * @brief Single-table mapper for MountRoot rows and MountRootRecord values.
 */
class MountRootMapper
    : public Mapper<MountRootMapper, MountRootRecord, MountRootMapperParams, mount_id_t>,
      public FieldReflectable<MountRootMapper> {
 private:
  static constexpr uint32_t                                         field_count_ = 6;
  static constexpr const char*                                      table_name_  = "MountRoot";
  static constexpr const char*                                      primary_key_column_ = "id";
  // Order matches the `MountRoot` table DDL, because duckorm select reads `SELECT *`.
  static constexpr std::array<duckorm::DuckFieldDesc, field_count_> field_descs_        = {
      FIELD(MountRootMapperParams, id, UINT32),
      FIELD(MountRootMapperParams, root_folder_id, UINT32),
      FIELD(MountRootMapperParams, volume_identity, NULLABLE_STRING),
      FIELD(MountRootMapperParams, root_path, STRING),
      FIELD(MountRootMapperParams, last_scan_at, NULLABLE_STRING),
      FIELD(MountRootMapperParams, volume_name, NULLABLE_STRING)};

 public:
  static auto FromRawData(std::vector<duckorm::VarTypes>&& data) -> MountRootMapperParams;
  static auto ToParams(const MountRootRecord& source) -> MountRootMapperParams;
  static auto FromParams(MountRootMapperParams&& param) -> MountRootRecord;

  friend struct FieldReflectable<MountRootMapper>;
  using Mapper::Mapper;
};
}  // namespace alcedo
