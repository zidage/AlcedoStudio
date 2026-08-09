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

// CREATE TABLE ImageEditState (
//   element_id BIGINT PRIMARY KEY,
//   root_id VARCHAR NOT NULL,
//   active_version_id VARCHAR NOT NULL,
//   materialized_head_commit_hash VARCHAR,
//   materialized_transaction_chain_hash VARCHAR NOT NULL,
//   serialized_pipeline_state JSON,
//   project_schema_version INTEGER NOT NULL);
struct ImageEditStateMapperParams {
  sl_element_id_t              element_id = 0;
  std::unique_ptr<std::string> root_id;
  std::unique_ptr<std::string> active_version_id;
  std::unique_ptr<std::string> materialized_head_commit_hash;
  std::unique_ptr<std::string> materialized_transaction_chain_hash;
  std::unique_ptr<std::string> serialized_pipeline_state;
  std::uint32_t                project_schema_version = 0;
};

class ImageEditStateMapper
    : public Mapper<ImageEditStateMapper, ImageEditStateMapperParams, ImageEditStateMapperParams, sl_element_id_t>,
      public FieldReflectable<ImageEditStateMapper> {
 private:
  static constexpr uint32_t    field_count_      = 7;
  static constexpr const char* table_name_       = "ImageEditState";
  static constexpr const char* prime_key_clause_ = "element_id={}";
  static constexpr std::array<duckorm::DuckFieldDesc, field_count_> field_descs_ = {
      FIELD(ImageEditStateMapperParams, element_id, UINT32),
      FIELD(ImageEditStateMapperParams, root_id, VARCHAR),
      FIELD(ImageEditStateMapperParams, active_version_id, VARCHAR),
      FIELD(ImageEditStateMapperParams, materialized_head_commit_hash, VARCHAR),
      FIELD(ImageEditStateMapperParams, materialized_transaction_chain_hash, VARCHAR),
      FIELD(ImageEditStateMapperParams, serialized_pipeline_state, JSON),
      FIELD(ImageEditStateMapperParams, project_schema_version, UINT32)};

 public:
  static auto FromRawData(std::vector<duckorm::VarTypes>&& data) -> ImageEditStateMapperParams;
  friend struct FieldReflectable<ImageEditStateMapper>;
  using Mapper::Mapper;
};

}  // namespace alcedo
