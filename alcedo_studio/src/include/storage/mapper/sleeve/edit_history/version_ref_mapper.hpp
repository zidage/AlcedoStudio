//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>

#include "storage/mapper/duckorm/duckdb_types.hpp"
#include "storage/mapper/mapper.hpp"
#include "type/type.hpp"

namespace alcedo {

// CREATE TABLE VersionRef (
//   version_id VARCHAR PRIMARY KEY,
//   element_id BIGINT NOT NULL,
//   display_name VARCHAR NOT NULL,
//   head_commit_hash VARCHAR,
//   created_at_unix BIGINT NOT NULL,
//   updated_at_unix BIGINT NOT NULL);
struct VersionRefMapperParams {
  std::unique_ptr<std::string> version_id;
  sl_element_id_t              element_id = 0;
  std::unique_ptr<std::string> display_name;
  std::unique_ptr<std::string> head_commit_hash;
  std::int64_t                 created_at_unix = 0;
  std::int64_t                 updated_at_unix = 0;
};

class VersionRefMapper
    : public Mapper<VersionRefMapper, VersionRefMapperParams, VersionRefMapperParams, std::string>,
      public FieldReflectable<VersionRefMapper> {
 private:
  static constexpr uint32_t    field_count_      = 6;
  static constexpr const char* table_name_       = "VersionRef";
  static constexpr const char* prime_key_clause_ = "version_id='{}'";
  static constexpr std::array<duckorm::DuckFieldDesc, field_count_> field_descs_ = {
      FIELD(VersionRefMapperParams, version_id, VARCHAR),
      FIELD(VersionRefMapperParams, element_id, UINT32),
      FIELD(VersionRefMapperParams, display_name, VARCHAR),
      FIELD(VersionRefMapperParams, head_commit_hash, VARCHAR),
      FIELD(VersionRefMapperParams, created_at_unix, INT64),
      FIELD(VersionRefMapperParams, updated_at_unix, INT64)};

 public:
  static auto FromRawData(std::vector<duckorm::VarTypes>&& data) -> VersionRefMapperParams;
  friend struct FieldReflectable<VersionRefMapper>;
  using Mapper::Mapper;
};

}  // namespace alcedo
