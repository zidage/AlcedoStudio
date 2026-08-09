//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <array>
#include <span>
#include <utility>
#include <vector>

#include "storage/mapper/duckorm/duckdb_types.hpp"
#include "storage/mapper/mapper.hpp"
#include "type/type.hpp"

namespace alcedo {
// CREATE TABLE FolderContent (folder_id BIGINT, element_id BIGINT);
struct FolderMapperParams {
  sl_element_id_t folder_id;
  sl_element_id_t element_id;
};

/**
 * @brief Single-table mapper for FolderContent membership rows.
 */
class FolderMapper
    : public Mapper<FolderMapper, std::pair<sl_element_id_t, sl_element_id_t>, FolderMapperParams,
                    sl_element_id_t>,
      public FieldReflectable<FolderMapper> {
 private:
  static constexpr uint32_t    field_count_                                      = 2;
  static constexpr const char* table_name_                                       = "FolderContent";
  static constexpr const char* prime_key_clause_                                 = "folder_id={}";
  static constexpr std::array<duckorm::DuckFieldDesc, field_count_> field_descs_ = {
      FIELD(FolderMapperParams, folder_id, UINT32), FIELD(FolderMapperParams, element_id, UINT32)};

 public:
  static auto FromRawData(std::vector<duckorm::VarTypes>&& data) -> FolderMapperParams;
  static auto ToParams(const std::pair<sl_element_id_t, sl_element_id_t> source)
      -> FolderMapperParams;
  static auto FromParams(FolderMapperParams&& param) -> std::pair<sl_element_id_t, sl_element_id_t>;

  auto        GetFolderContent(const sl_element_id_t id) -> std::vector<sl_element_id_t>;
  void        RemoveAllContents(const sl_element_id_t folder_id);
  void        RemoveContentById(const sl_element_id_t content_id);
  void        RemoveContentByIds(std::span<const sl_element_id_t> content_ids);
  void        RemoveFolderContent(sl_element_id_t folder_id, sl_element_id_t content_id);
  void        UpdateFolderContent(const std::vector<sl_element_id_t>& content,
                                  const sl_element_id_t               folder_id);

  friend struct FieldReflectable<FolderMapper>;
  using Mapper::Mapper;
};
}  // namespace alcedo
