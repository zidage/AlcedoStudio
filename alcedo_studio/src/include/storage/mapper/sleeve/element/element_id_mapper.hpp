//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <array>
#include <vector>

#include "storage/mapper/duckorm/duckdb_types.hpp"
#include "storage/mapper/mapper.hpp"
#include "type/type.hpp"

namespace alcedo {
// Projection mapper for queries that only return Element.id.
struct ElementIdMapperParams {
  sl_element_id_t id;
};

/**
 * @brief Projection mapper for Element.id columns returned by filter queries.
 */
class ElementIdMapper
    : public Mapper<ElementIdMapper, sl_element_id_t, ElementIdMapperParams, sl_element_id_t>,
      public FieldReflectable<ElementIdMapper> {
 private:
  static constexpr uint32_t                                         field_count_      = 1;
  static constexpr const char*                                      table_name_       = "Element";
  static constexpr const char*                                      prime_key_clause_ = "id={}";

  static constexpr std::array<duckorm::DuckFieldDesc, field_count_> field_descs_ = {
      FIELD(ElementIdMapperParams, id, UINT32)};

 public:
  static auto FromRawData(std::vector<duckorm::VarTypes>&& data) -> ElementIdMapperParams;
  static auto ToParams(const sl_element_id_t& source) -> ElementIdMapperParams;
  static auto FromParams(ElementIdMapperParams&& param) -> sl_element_id_t;

  /// Specialized queries only: expects a single UINT32 column named "id".
  auto GetElementIdsByQuery(const std::wstring& query_sql) -> std::vector<sl_element_id_t>;

  friend struct FieldReflectable<ElementIdMapper>;
  using Mapper::Mapper;
};
}  // namespace alcedo
