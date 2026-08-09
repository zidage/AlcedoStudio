//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <array>

#include "storage/mapper/duckorm/duckdb_types.hpp"
#include "storage/mapper/mapper.hpp"
#include "type/type.hpp"

namespace alcedo {
// CREATE TABLE Sleeve (id BIGINT PRIMARY KEY);
struct BaseMapperParams {
  sleeve_id_t id;
};

/**
 * @brief Single-table mapper for the Sleeve identity row.
 */
class BaseMapper : public Mapper<BaseMapper, sleeve_id_t, BaseMapperParams, sleeve_id_t>,
                   public FieldReflectable<BaseMapper> {
 private:
  static constexpr uint32_t                                         field_count_      = 1;
  static constexpr const char*                                      table_name_       = "Sleeve";
  static constexpr const char*                                      prime_key_clause_ = "id={}";

  static constexpr std::array<duckorm::DuckFieldDesc, field_count_> field_descs_ = {
      FIELD(BaseMapperParams, id, UINT32)};

 public:
  static auto FromRawData(std::vector<duckorm::VarTypes>&& data) -> BaseMapperParams;
  static auto ToParams(const sleeve_id_t source) -> BaseMapperParams;
  static auto FromParams(BaseMapperParams&& param) -> sleeve_id_t;

  friend struct FieldReflectable<BaseMapper>;
  using Mapper::Mapper;
};

// CREATE TABLE SleeveRoot (id BIGINT PRIMARY KEY);"
struct RootMapperParams {
  sl_element_id_t id_;
};

/**
 * @brief Single-table mapper for the SleeveRoot row.
 */
class RootMapper : public Mapper<RootMapper, sl_element_id_t, RootMapperParams, sl_element_id_t>,
                   public FieldReflectable<RootMapper> {
 private:
  static constexpr uint32_t                                         field_count_      = 1;
  static constexpr const char*                                      table_name_       = "SleeveRoot";
  static constexpr const char*                                      prime_key_clause_ = "id={}";

  static constexpr std::array<duckorm::DuckFieldDesc, field_count_> field_descs_ = {
      FIELD(RootMapperParams, id_, UINT32)};

 public:
  static auto FromRawData(std::vector<duckorm::VarTypes>&& data) -> RootMapperParams;
  static auto ToParams(const sl_element_id_t source) -> RootMapperParams;
  static auto FromParams(RootMapperParams&& param) -> sl_element_id_t;

  friend struct FieldReflectable<RootMapper>;
  using Mapper::Mapper;
};
}  // namespace alcedo
