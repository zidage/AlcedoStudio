//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <array>
#include <memory>

#include "edit/history/edit_history.hpp"
#include "storage/mapper/duckorm/duckdb_types.hpp"
#include "storage/mapper/mapper.hpp"
#include "type/type.hpp"

namespace alcedo {
// CREATE TABLE EditHistory (file_id PRIMARY KEY BIGINT, history JSON);
struct EditHistoryMapperParams {
  sl_element_id_t              file_id;
  std::unique_ptr<std::string> history;
};

/**
 * @brief Single-table mapper for EditHistory rows and domain EditHistory objects.
 */
class EditHistoryMapper
    : public Mapper<EditHistoryMapper, std::shared_ptr<EditHistory>, EditHistoryMapperParams,
                    sl_element_id_t>,
      public FieldReflectable<EditHistoryMapper> {
 private:
  static constexpr uint32_t    field_count_                                     = 2;
  static constexpr const char* table_name_                                      = "EditHistory";
  static constexpr const char* prime_key_clause_                                = "file_id={}";
  static constexpr std::array<duckorm::DuckFieldDesc, field_count_> field_descs_ = {
      FIELD(EditHistoryMapperParams, file_id, UINT32),
      FIELD(EditHistoryMapperParams, history, VARCHAR)};

 public:
  static auto FromRawData(std::vector<duckorm::VarTypes>&& data) -> EditHistoryMapperParams;
  static auto ToParams(const std::shared_ptr<EditHistory> source) -> EditHistoryMapperParams;
  static auto FromParams(EditHistoryMapperParams&& param) -> std::shared_ptr<EditHistory>;

  auto GetEditHistoryByFileId(const sl_element_id_t file_id) -> std::shared_ptr<EditHistory>;
  void UpdateEditHistory(const std::shared_ptr<EditHistory> history);

  friend struct FieldReflectable<EditHistoryMapper>;
  using Mapper::Mapper;
};
}  // namespace alcedo
