//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <array>
#include <cstdint>
#include <utility>

#include "storage/mapper/duckorm/duckdb_types.hpp"
#include "storage/mapper/mapper.hpp"
#include "type/type.hpp"

namespace alcedo {
// CREATE TABLE FileImage (file_id BIGINT, image_id BIGINT);
struct FileMapperParams {
  sl_element_id_t file_id;
  image_id_t      image_id;
};

/**
 * @brief Single-table mapper for FileImage bindings (file_id → image_id).
 */
class FileMapper
    : public Mapper<FileMapper, std::pair<sl_element_id_t, image_id_t>, FileMapperParams,
                    sl_element_id_t>,
      public FieldReflectable<FileMapper> {
 private:
  static constexpr uint32_t    field_count_                                      = 2;
  static constexpr const char* table_name_                                       = "FileImage";
  static constexpr const char* prime_key_clause_                                 = "file_id={}";
  static constexpr std::array<duckorm::DuckFieldDesc, field_count_> field_descs_ = {
      FIELD(FileMapperParams, file_id, UINT32), FIELD(FileMapperParams, image_id, UINT32)};

 public:
  static auto FromRawData(std::vector<duckorm::VarTypes>&& data) -> FileMapperParams;
  static auto ToParams(const std::pair<sl_element_id_t, image_id_t>& source) -> FileMapperParams;
  static auto FromParams(FileMapperParams&& param) -> std::pair<sl_element_id_t, image_id_t>;

  auto        GetFileById(const sl_element_id_t id) -> std::pair<sl_element_id_t, image_id_t>;
  auto        GetBoundImageById(const sl_element_id_t id) -> image_id_t;
  void        RemoveBindByFileId(const sl_element_id_t id);

  friend struct FieldReflectable<FileMapper>;
  using Mapper::Mapper;
};
}  // namespace alcedo
