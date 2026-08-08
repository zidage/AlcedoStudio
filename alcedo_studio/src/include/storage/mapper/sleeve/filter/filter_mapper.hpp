//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <array>
#include <cstdint>

#include "storage/mapper/duckorm/duckdb_types.hpp"
#include "storage/mapper/mapper.hpp"
#include "type/type.hpp"

namespace alcedo {
// CREATE TABLE Filter (combo_id BIGINT, type INTEGER, data JSON);
struct FilterMapperParams {
  uint32_t    combo_id;
  uint32_t    type;
  const char* data;
};
class FilterMapper : Mapper<FilterMapper, FilterMapperParams, FilterMapperParams, sl_element_id_t>,
                     FieldReflectable<FilterMapper> {
 private:
  static auto FromRawData(std::vector<duckorm::VarTypes>&& data) -> FilterMapperParams;
  static constexpr uint32_t    field_count_                                      = 3;
  static constexpr const char* table_name_                                       = "Filter";
  static constexpr const char* prime_key_clause_                                 = "combo_id={}";
  static constexpr std::array<duckorm::DuckFieldDesc, field_count_> field_descs_ = {
      FIELD(FilterMapperParams, combo_id, UINT32), FIELD(FilterMapperParams, type, UINT32),
      FIELD(FilterMapperParams, data, VARCHAR)};

 public:
  using Mapper::Mapper;
  friend struct FieldReflectable<FilterMapper>;
};

// CREATE TABLE ComboFolder (combo_id BIGINT, folder_id BIGINT);
struct ComboMapperParams {
  uint32_t combo_id_;
  uint32_t folder_id_;
};
class ComboMapper : Mapper<ComboMapper, ComboMapperParams, ComboMapperParams, sl_element_id_t>,
                    FieldReflectable<ComboMapper> {
 private:
  static constexpr uint32_t    field_count_                                      = 2;
  static constexpr const char* table_name_                                       = "ComboFolder";
  static constexpr const char* prime_key_clause_                                 = "combo_id={}";
  static constexpr std::array<duckorm::DuckFieldDesc, field_count_> field_descs_ = {
      FIELD(ComboMapperParams, combo_id_, UINT32), FIELD(ComboMapperParams, folder_id_, UINT32)};

 public:
  static auto FromRawData(std::vector<duckorm::VarTypes>&& data) -> ComboMapperParams;
  friend struct FieldReflectable<ComboMapper>;
  using Mapper::Mapper;
};
};  // namespace alcedo