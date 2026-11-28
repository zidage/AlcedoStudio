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
#include "type/source_file_record.hpp"
#include "type/type.hpp"

namespace alcedo {
/// One SourceFile row (see `mount_schema_query` in database.hpp), in table order.
struct SourceFileMapperParams {
  sl_element_id_t element_id  = 0;
  int64_t         byte_size   = 0;
  int64_t         modified_ns = 0;
  /// Hash128::ToString text (32 hexadecimal digits).
  std::string     fingerprint;
  int32_t         fingerprint_version = 0;
  /// SourceFileState code.
  int32_t         state               = 0;
  /// `YYYY-MM-DD HH:MM:SS` UTC; empty writes NULL.
  std::string     missing_since;
};

/**
 * @brief Single-table mapper for SourceFile rows and SourceFileRecord values.
 */
class SourceFileMapper
    : public Mapper<SourceFileMapper, SourceFileRecord, SourceFileMapperParams, sl_element_id_t>,
      public FieldReflectable<SourceFileMapper> {
 private:
  static constexpr uint32_t    field_count_                                      = 7;
  static constexpr const char* table_name_                                       = "SourceFile";
  static constexpr const char* primary_key_column_                               = "element_id";
  // Order matches the `SourceFile` table DDL, because duckorm select reads `SELECT *`.
  static constexpr std::array<duckorm::DuckFieldDesc, field_count_> field_descs_ = {
      FIELD(SourceFileMapperParams, element_id, UINT32),
      FIELD(SourceFileMapperParams, byte_size, INT64),
      FIELD(SourceFileMapperParams, modified_ns, INT64),
      FIELD(SourceFileMapperParams, fingerprint, STRING),
      FIELD(SourceFileMapperParams, fingerprint_version, INT32),
      FIELD(SourceFileMapperParams, state, INT32),
      FIELD(SourceFileMapperParams, missing_since, NULLABLE_STRING)};

 public:
  static auto FromRawData(std::vector<duckorm::VarTypes>&& data) -> SourceFileMapperParams;
  /// @throws std::invalid_argument when the byte size does not fit BIGINT, or the state and
  ///         `missing_since` disagree (see SourceFileRecord).
  static auto ToParams(const SourceFileRecord& source) -> SourceFileMapperParams;
  /// @throws std::runtime_error when the stored row holds a value that no record can hold.
  static auto FromParams(SourceFileMapperParams&& param) -> SourceFileRecord;

  friend struct FieldReflectable<SourceFileMapper>;
  using Mapper::Mapper;
};
}  // namespace alcedo
