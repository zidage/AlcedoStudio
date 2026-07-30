//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>

#include "storage/mapper/duckorm/duckdb_types.hpp"
#include "storage/mapper/mapper_interface.hpp"
#include "type/type.hpp"

namespace alcedo {

// CREATE TABLE EditorRecoveryMetadata (
//   file_id BIGINT PRIMARY KEY,
//   version_id VARCHAR,
//   journal_generation UBIGINT,
//   materialized_operation_sequence UBIGINT,
//   transaction_chain_hash VARCHAR,
//   pipeline_parameter_hash VARCHAR);
struct EditorRecoveryMetadataMapperParams {
  sl_element_id_t              file_id = 0;
  std::unique_ptr<std::string> version_id;
  std::uint64_t                journal_generation              = 0;
  std::uint64_t                materialized_operation_sequence = 0;
  std::unique_ptr<std::string> transaction_chain_hash;
  std::unique_ptr<std::string> pipeline_parameter_hash;
};

class EditorRecoveryMetadataMapper
    : public MapperInterface<EditorRecoveryMetadataMapper, EditorRecoveryMetadataMapperParams,
                             sl_element_id_t>,
      public FieldReflectable<EditorRecoveryMetadataMapper> {
 private:
  static constexpr uint32_t    field_count_      = 6;
  static constexpr const char* table_name_       = "EditorRecoveryMetadata";
  static constexpr const char* prime_key_clause_ = "file_id={}";
  static constexpr std::array<duckorm::DuckFieldDesc, field_count_> field_descs_ = {
      FIELD(EditorRecoveryMetadataMapperParams, file_id, UINT32),
      FIELD(EditorRecoveryMetadataMapperParams, version_id, VARCHAR),
      FIELD(EditorRecoveryMetadataMapperParams, journal_generation, UINT64),
      FIELD(EditorRecoveryMetadataMapperParams, materialized_operation_sequence, UINT64),
      FIELD(EditorRecoveryMetadataMapperParams, transaction_chain_hash, VARCHAR),
      FIELD(EditorRecoveryMetadataMapperParams, pipeline_parameter_hash, VARCHAR)};

 public:
  static auto FromRawData(std::vector<duckorm::VarTypes>&& data)
      -> EditorRecoveryMetadataMapperParams;
  friend struct FieldReflectable<EditorRecoveryMetadataMapper>;
  using MapperInterface::MapperInterface;
};

}  // namespace alcedo
