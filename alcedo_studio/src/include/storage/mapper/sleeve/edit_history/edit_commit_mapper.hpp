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

namespace alcedo {

// CREATE TABLE EditCommit (
//   commit_hash VARCHAR PRIMARY KEY,
//   root_id VARCHAR NOT NULL,
//   first_parent_hash VARCHAR,
//   second_parent_hash VARCHAR,
//   created_at_ns UBIGINT NOT NULL,
//   kind INTEGER NOT NULL,
//   edit_payload JSON NOT NULL);
struct EditCommitMapperParams {
  std::unique_ptr<std::string> commit_hash;
  std::unique_ptr<std::string> root_id;
  std::unique_ptr<std::string> first_parent_hash;
  std::unique_ptr<std::string> second_parent_hash;
  std::uint64_t                created_at_ns = 0;
  std::uint32_t                kind          = 0;
  std::unique_ptr<std::string> edit_payload;
};

class EditCommitMapper
    : public MapperInterface<EditCommitMapper, EditCommitMapperParams, std::string>,
      public FieldReflectable<EditCommitMapper> {
 private:
  static constexpr uint32_t    field_count_      = 7;
  static constexpr const char* table_name_       = "EditCommit";
  static constexpr const char* prime_key_clause_ = "commit_hash='{}'";
  static constexpr std::array<duckorm::DuckFieldDesc, field_count_> field_descs_ = {
      FIELD(EditCommitMapperParams, commit_hash, VARCHAR),
      FIELD(EditCommitMapperParams, root_id, VARCHAR),
      FIELD(EditCommitMapperParams, first_parent_hash, VARCHAR),
      FIELD(EditCommitMapperParams, second_parent_hash, VARCHAR),
      FIELD(EditCommitMapperParams, created_at_ns, UINT64),
      FIELD(EditCommitMapperParams, kind, UINT32),
      FIELD(EditCommitMapperParams, edit_payload, JSON)};

 public:
  static auto FromRawData(std::vector<duckorm::VarTypes>&& data) -> EditCommitMapperParams;
  friend struct FieldReflectable<EditCommitMapper>;
  using MapperInterface::MapperInterface;
};

}  // namespace alcedo
