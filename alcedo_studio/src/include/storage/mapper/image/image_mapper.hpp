//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "image/image.hpp"
#include "storage/mapper/duckorm/duckdb_types.hpp"
#include "storage/mapper/mapper.hpp"
#include "type/type.hpp"

namespace alcedo {
// CREATE TABLE Image (id BIGINT PRIMARY KEY, image_path TEXT, file_name TEXT, type INTEGER,
// metadata JSON);
struct ImageMapperParams {
  image_id_t                   id;
  std::unique_ptr<std::string> image_path;
  std::unique_ptr<std::string> file_name;
  uint32_t                     type;
  std::unique_ptr<std::string> metadata;
};

/**
 * @brief Single-table mapper for Image rows and domain Image objects.
 */
class ImageMapper
    : public Mapper<ImageMapper, std::shared_ptr<Image>, ImageMapperParams, image_id_t>,
      public FieldReflectable<ImageMapper> {
 private:
  static constexpr uint32_t                                         field_count_      = 5;
  static constexpr const char*                                      table_name_       = "Image";
  static constexpr const char*                                      prime_key_clause_ = "id={}";
  static constexpr std::array<duckorm::DuckFieldDesc, field_count_> field_descs_      = {
      FIELD(ImageMapperParams, id, UINT32), FIELD(ImageMapperParams, image_path, VARCHAR),
      FIELD(ImageMapperParams, file_name, VARCHAR), FIELD(ImageMapperParams, type, UINT32),
      FIELD(ImageMapperParams, metadata, VARCHAR)};

 public:
  static auto FromRawData(std::vector<duckorm::VarTypes>&& data) -> ImageMapperParams;
  static auto ToParams(const std::shared_ptr<Image> source) -> ImageMapperParams;
  static auto FromParams(ImageMapperParams&& param) -> std::shared_ptr<Image>;

  auto        GetImageById(const image_id_t id) -> std::vector<std::shared_ptr<Image>>;
  auto        GetImageByName(const std::wstring& name) -> std::vector<std::shared_ptr<Image>>;
  auto GetImageByPath(const std::filesystem::path path) -> std::vector<std::shared_ptr<Image>>;
  auto GetImageByType(const ImageType type) -> std::vector<std::shared_ptr<Image>>;

  friend struct FieldReflectable<ImageMapper>;
  using Mapper::Mapper;
};
}  // namespace alcedo
