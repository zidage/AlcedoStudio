//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "sleeve/sleeve_element/sleeve_element.hpp"
#include "storage/mapper/duckorm/duckdb_types.hpp"
#include "storage/mapper/mapper.hpp"
#include "type/type.hpp"

namespace alcedo {
// Element (id BIGINT PRIMARY KEY, type INTEGER, element_name TEXT, added_time "
//     "TIMESTAMP, modified_time "
//     "TIMESTAMP, "
//     "ref_count BIGINT);"
struct ElementMapperParams {
  sl_element_id_t              id;
  uint32_t                     type;
  std::unique_ptr<std::string> element_name;
  std::unique_ptr<std::string> added_time;
  std::unique_ptr<std::string> modified_time;
  uint32_t                     ref_count;
};

/**
 * @brief Single-table mapper for Sleeve Element rows and domain SleeveElement objects.
 */
class ElementMapper
    : public Mapper<ElementMapper, std::shared_ptr<SleeveElement>, ElementMapperParams,
                    sl_element_id_t>,
      public FieldReflectable<ElementMapper> {
 private:
  static constexpr uint32_t                                         field_count_      = 6;
  static constexpr const char*                                      table_name_       = "Element";
  static constexpr const char*                                      prime_key_clause_ = "id={}";
  static constexpr std::array<duckorm::DuckFieldDesc, field_count_> field_descs_      = {
      FIELD(ElementMapperParams, id, UINT32),
      FIELD(ElementMapperParams, type, UINT32),
      FIELD(ElementMapperParams, element_name, VARCHAR),
      FIELD(ElementMapperParams, added_time, TIMESTAMP),
      FIELD(ElementMapperParams, modified_time, TIMESTAMP),
      FIELD(ElementMapperParams, ref_count, UINT32)};

 public:
  static auto FromRawData(std::vector<duckorm::VarTypes>&& data) -> ElementMapperParams;
  static auto ToParams(const std::shared_ptr<SleeveElement>& source) -> ElementMapperParams;
  static auto FromParams(ElementMapperParams&& param) -> std::shared_ptr<SleeveElement>;

  auto        GetElementById(const sl_element_id_t id) -> std::shared_ptr<SleeveElement>;
  auto GetElementByName(const std::wstring& name) -> std::vector<std::shared_ptr<SleeveElement>>;
  auto GetElementByType(const ElementType type) -> std::vector<std::shared_ptr<SleeveElement>>;
  auto GetElementsInFolderByFilter(const std::wstring& filter_sql)
      -> std::vector<std::shared_ptr<SleeveElement>>;

  friend struct FieldReflectable<ElementMapper>;
  using Mapper::Mapper;
};
}  // namespace alcedo
